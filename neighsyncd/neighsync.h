#ifndef __NEIGHSYNC__
#define __NEIGHSYNC__

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include <unordered_map>
#include <string>

#define DEFAULT_RECONCILE_TIMER 5
#define SELECT_TIMEOUT 1000
#define CACHE_STATE	"cache-state"
#define CFG_WARMSTART_TABLE_NAME "WARMSTART_TABLE"

typedef std::unordered_map<std::string, std::vector<swss::FieldValueTuple>> NeighborTableMap;

namespace swss {

/*
 * The intention of this class is to support any daemon/table
 * restart/reconciliation. You can load the map with any table name.
 * It should be moved to swss-common once it is ready.
 * It supports neighbor table only for now.
 */
class NeighRestartAssit
{
public:
    NeighRestartAssit(RedisPipeline *pipelineAppDB, RedisPipeline *pipelineConfDB);
    virtual ~NeighRestartAssit();

    enum cache_state_t 
    {
	STALE	= 0,
	SAME 	= 1,
	NEW 	= 2,
        DELETE  = 3,
	UNKNOWN = 4
    };
    bool checkReconcile();
    void readTableToMap();
    void insertToMap(std::string key, std::vector<FieldValueTuple> fvVector, bool delete_key);
    void reconcile(ProducerStateTable &ps_table);
    bool m_warmStartInProgress = true;

private:
    bool m_table_loaded = false;
    unsigned int m_reconcileTimer = DEFAULT_RECONCILE_TIMER;
    time_t m_startTime;
    double m_secondsPast = 0;
    NeighborTableMap neighborCacheMap;
    std::string m_appTableName;
    Table m_appTable;
    Table m_cfgTable;
    std::string state_string[5] = {"STALE", "SAME", "NEW", "DELETE", "UNKNOWN"};
    void readTableToMapExec();
    std::string joinVectorString(std::vector<FieldValueTuple> fv);
    void setCacheEntryState(std::vector<FieldValueTuple> &fvVector, cache_state_t state);
    cache_state_t getCacheEntryState(const std::vector<FieldValueTuple> &fvVector);

//protected:
//    ProducerStateTable m_neighTable;
};

class NeighSync : public NetMsg, public NeighRestartAssit
{
public:
    enum { MAX_ADDR_SIZE = 64 };

    NeighSync(RedisPipeline *pipelineAppDB,
      RedisPipeline *pipelineConfDB);

    virtual void onMsg(int nlmsg_type, struct nl_object *obj);

    ProducerStateTable &get_ps_table() {
	return m_neighTable;
    }
private:
    ProducerStateTable m_neighTable;
};

}

#endif
