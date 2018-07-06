#include <string>
#include <netinet/in.h>
#include <netlink/route/link.h>
#include <netlink/route/neighbour.h>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "ipaddress.h"
#include "netmsg.h"
#include "linkcache.h"

#include "neighsync.h"
#include <climits>
#include "warm_restart.h"

using namespace std;
using namespace swss;

NeighRestartAssit::NeighRestartAssit(RedisPipeline *pipelineAppDB,
  RedisPipeline *pipelineConfDB):
    m_appTable(pipelineAppDB, APP_NEIGH_TABLE_NAME, false),
    m_cfgTable(pipelineConfDB, CFG_WARM_RESTART_TABLE_NAME, false)
{
    m_startTime = time(NULL);
    m_appTableName = m_appTable.getTableName();

    if (!isWarmStart())
    {
        m_warmStartInProgress = false;
    }
    else
    {
        vector<FieldValueTuple> fvs;
        if (m_cfgTable.get("swss", fvs))
        {
            string s = "";
            s = joinVectorString(fvs);
            SWSS_LOG_NOTICE("read configDB %s swss: %s", m_cfgTable.getTableName().c_str(), s.c_str());
            for (const auto &fv : fvs)
            {
                if (fv.first == "neighbor_timer")
                {
                    long int temp = strtol(fv.second.c_str(), NULL, 0);
                    if (temp != 0 && temp != LONG_MIN && temp != LONG_MAX)
                    {
                        m_reconcileTimer = (unsigned int) temp;
                    }
                }
            }
        }
    }
}

NeighRestartAssit::~NeighRestartAssit()
{
}

/* join the field-value strings for straight printing */
string NeighRestartAssit::joinVectorString(const vector<FieldValueTuple> &fv)
{
    string s;
    for (const auto &temps : fv )
    {
	   s += temps.first + ":" + temps.second + ", ";
    }
    return s;
}

void NeighRestartAssit::setCacheEntryState(std::vector<FieldValueTuple> &fvVector,
    cache_state_t state)
{
    fvVector.back().second = NeighRestartAssit::state_string[state];
}

NeighRestartAssit::cache_state_t NeighRestartAssit::getCacheEntryState(const std::vector<FieldValueTuple> &fvVector)
{
    if (fvVector.back().second == "STALE")
    {
	   return NeighRestartAssit::STALE;
    }
    else if (fvVector.back().second == "SAME")
    {
	   return NeighRestartAssit::SAME;
    }
    else if (fvVector.back().second == "NEW")
    {
	   return NeighRestartAssit::NEW;
    }
    else if (fvVector.back().second == "DELETE")
    {
	   return NeighRestartAssit::DELETE;
    }
    else
    {
        /* should not reach here */
        return NeighRestartAssit::UNKNOWN;
    }
}

/* Read table from APPDB and append stale flag then insert to cachemap */
void NeighRestartAssit::readTableToMapExec()
{
    vector<string> keys;

    m_appTable.getKeys(keys);
    FieldValueTuple state(CACHE_STATE, "UNKNOWN");

    for (const auto &key: keys)
    {
        vector<FieldValueTuple> fv;

	    /* if the fieldvlaue empty, skip */
        if (!m_appTable.get(key, fv))
        {
            continue;
        }

        fv.push_back(state);
        setCacheEntryState(fv, STALE);

        string s = "";
        s = joinVectorString(fv);

        SWSS_LOG_INFO("write to cachemap: %s, key: %s, "
               "%s", m_appTableName.c_str(), key.c_str(), s.c_str());

        // insert to the cache map
        neighborCacheMap[key] = fv;
    }
    return;
}

void NeighRestartAssit::readTableToMap()
{
    if (m_warmStartInProgress)
    {
        if (!m_table_loaded)
        {
            // read table to cache map with stale flag.
            readTableToMapExec();
            m_table_loaded = true;
            SWSS_LOG_NOTICE("Restored appDB table to internal cache map");
        }
    }
    return;
}

void NeighRestartAssit::insertToMap(string key, vector<FieldValueTuple> fvVector, bool delete_key)
{
    string s;
    s = joinVectorString(fvVector);

    SWSS_LOG_INFO("Received message %s, key: %s, "
            "%s, delete = %d", m_appTableName.c_str(), key.c_str(), s.c_str(), delete_key);

    /*
     * if delete_key, mark the entry as delete;
     * else:
     *  if key exist {
     *    if different value: update with new flag.
     *    if same value:  mark it as same;
     *  } else {
     *    insert with new flag.
     *   }
     */

    NeighborTableMap::iterator found = neighborCacheMap.find(key);

    if (delete_key)
    {
        SWSS_LOG_NOTICE("%s, delete key: %s, ", m_appTableName.c_str(), key.c_str());
        /* mark it as DELETE if exist, otherwise, no-op */
        if (found != neighborCacheMap.end())
        {
            setCacheEntryState(found->second, DELETE);
        }
    }
    else if (found != neighborCacheMap.end())
    {
        /* check only the original vector range (exclude stale flag) */
        if(!equal(fvVector.begin(), fvVector.end(), found->second.begin()))
        {
            SWSS_LOG_NOTICE("%s, found key: %s, new value ", m_appTableName.c_str(), key.c_str());

            FieldValueTuple state(CACHE_STATE, "UNKNOWN");
            fvVector.push_back(state);

            //mark as NEW flag
            setCacheEntryState(fvVector, NEW);
            neighborCacheMap[key] = fvVector;
        }
        else
        {
            SWSS_LOG_INFO("%s, found key: %s, same value", m_appTableName.c_str(), key.c_str());

            //mark as SAME flag
            setCacheEntryState(found->second, SAME);
        }
    }
    else
    {
        SWSS_LOG_NOTICE("%s, not found key: %s, new", m_appTableName.c_str(), key.c_str());
        FieldValueTuple state(CACHE_STATE, "UNKNOWN");
        fvVector.push_back(state);
        setCacheEntryState(fvVector, NEW);
        neighborCacheMap[key] = fvVector;
    }

    return;
}

void NeighRestartAssit::reconcile(ProducerStateTable &ps_table)
{
    /*
       iterate throught the table
       if it has SAME flag, do nothing
       if has stale/delete flag, delete from appDB.
       else if new flag,  add it to appDB
       else, assert (should not happen)
    */
    SWSS_LOG_NOTICE("Hit reconcile function");
    for (auto iter = neighborCacheMap.begin(); iter != neighborCacheMap.end(); ++iter )
    {
        string s = "";
        s = joinVectorString(iter->second);
        if (getCacheEntryState(iter->second) == NeighRestartAssit::SAME)
        {
            SWSS_LOG_INFO("%s SAME, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());
            continue;
        }
        else if (getCacheEntryState(iter->second) == NeighRestartAssit::STALE ||
            getCacheEntryState(iter->second) == NeighRestartAssit::DELETE)
        {
            SWSS_LOG_NOTICE("%s STALE/DELETE, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());

            //delete from appDB
            ps_table.del(iter->first);
        }
        else if (getCacheEntryState(iter->second) == NeighRestartAssit::NEW)
        {
            SWSS_LOG_NOTICE("%s NEW, key: %s, %s",
                    m_appTableName.c_str(), iter->first.c_str(), s.c_str());

            //add to appDB, exclude the state
            iter->second.pop_back();
            ps_table.set(iter->first, iter->second);
        }
        else
        {
            assert("unknown type cache state" == NULL);
        }
    }
    return;
}

bool NeighRestartAssit::checkReconcile()
{
    if (m_warmStartInProgress)
    {
        m_secondsPast =  difftime(time(NULL), m_startTime);
        SWSS_LOG_INFO("restart timer past: %f seconds", m_secondsPast);
        if (m_secondsPast >= m_reconcileTimer)
        {
            m_warmStartInProgress = false;
            return true;
        }
    }
    return false;
}

NeighSync::NeighSync(RedisPipeline *pipelineAppDB,
    RedisPipeline *pipelineConfDB) :
    m_neighTable(pipelineAppDB, APP_NEIGH_TABLE_NAME),
    NeighRestartAssit(pipelineAppDB, pipelineConfDB)
{
}

void NeighSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    char ipStr[MAX_ADDR_SIZE + 1] = {0};
    char macStr[MAX_ADDR_SIZE + 1] = {0};
    struct rtnl_neigh *neigh = (struct rtnl_neigh *)obj;
    string key;
    string family;

    if ((nlmsg_type != RTM_NEWNEIGH) && (nlmsg_type != RTM_GETNEIGH) &&
        (nlmsg_type != RTM_DELNEIGH))
        return;

    if (rtnl_neigh_get_family(neigh) == AF_INET)
        family = IPV4_NAME;
    else if (rtnl_neigh_get_family(neigh) == AF_INET6)
        family = IPV6_NAME;
    else
        return;

    key+= LinkCache::getInstance().ifindexToName(rtnl_neigh_get_ifindex(neigh));
    key+= ":";

    nl_addr2str(rtnl_neigh_get_dst(neigh), ipStr, MAX_ADDR_SIZE);
    /* Ignore IPv6 link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
        return;
    /* Ignore IPv6 multicast link-local addresses as neighbors */
    if (family == IPV6_NAME && IN6_IS_ADDR_MC_LINKLOCAL(nl_addr_get_binary_addr(rtnl_neigh_get_dst(neigh))))
        return;
    key+= ipStr;

    int state = rtnl_neigh_get_state(neigh);
    bool delete_key = false;
    if ((nlmsg_type == RTM_DELNEIGH) || (state == NUD_INCOMPLETE) ||
        (state == NUD_FAILED))
    {
	   delete_key = true;
    }

    nl_addr2str(rtnl_neigh_get_lladdr(neigh), macStr, MAX_ADDR_SIZE);

    std::vector<FieldValueTuple> fvVector;
    FieldValueTuple f("family", family);
    FieldValueTuple nh("neigh", macStr);
    fvVector.push_back(nh);
    fvVector.push_back(f);

    if (m_warmStartInProgress)
    {
        insertToMap(key, fvVector, delete_key);
    }
    else
    {
        if (delete_key == true)
        {
            m_neighTable.del(key);
            return;
        }
        m_neighTable.set(key, fvVector);
    }
}
