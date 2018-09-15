#include <cassert>
#include <sstream>

#include "warmRestartHelper.h"


using namespace swss;


WarmStartHelper::WarmStartHelper(RedisPipeline      *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string  &syncTableName,
                                 const std::string  &dockerName,
                                 const std::string  &appName) :
    m_restorationTable(pipeline, syncTableName, false),
    m_syncTable(syncTable),
    m_syncTableName(syncTableName),
    m_dockName(dockerName),
    m_appName(appName)
{
    WarmStart::initialize(appName, dockerName);
}


WarmStartHelper::~WarmStartHelper()
{
}


void WarmStartHelper::setState(WarmStart::WarmStartState state)
{
    WarmStart::setWarmStartState(m_appName, state);

    /* Caching warm-restart FSM state in local member */
    m_state = state;
}


WarmStart::WarmStartState WarmStartHelper::getState(void) const
{
    return m_state;
}


/*
 * To be called by each application to obtain the active/inactive state of
 * warm-restart functionality, and proceed to initialize the FSM accordingly.
 */
bool WarmStartHelper::isEnabled(void)
{
    bool enabled = WarmStart::checkWarmStart(m_appName, m_dockName);

    /*
     * If warm-restart feature is enabled for this application, proceed to
     * initialize its FSM, and clean any pending state that could be potentially
     * held in ProducerState queues.
     */
    if (enabled)
    {
        setState(WarmStart::INITIALIZED);
        m_syncTable->clear();
    }

    /* Keeping track of warm-reboot active/inactive state */
    m_enabled = enabled;

    return enabled;
}


bool WarmStartHelper::isReconciled(void) const
{
    return (m_state == WarmStart::RECONCILED);
}


bool WarmStartHelper::inProgress(void) const
{
    return (m_enabled && m_state != WarmStart::RECONCILED);
}


uint32_t WarmStartHelper::getRestartTimer(void) const
{
    return WarmStart::getWarmStartTimer(m_appName, m_dockName);
}


void WarmStartHelper::reconcile(void)
{
    // Whether there is case to delete key first and set agin with new set of FV.
    bool del2set = false;

    for(auto kfv : m_restorationVector)
    {
        auto key = kfvKey(kfv);
        if(m_kfvMap.find(key) == m_kfvMap.end())
        {
            //Stale entry
            m_syncTable->del(kfvKey(kfv));
            continue;
        }
        KeyOpFieldsValuesTuple &newKfv =  m_kfvMap[key];

        if(kfvOp(newKfv) == DEL_COMMAND)
        {
            //Delete request from application
            m_syncTable->del(key);
            //Erase the enetry from kfv map
            m_kfvMap.erase(key);
            continue;
        }

        // Compare new and old kfv
        auto ret = compareFv(kfvFieldsValues(newKfv), kfvFieldsValues(kfv));
        if( ret > 0)
        {
            // There is change in field value,
            // and the all fields in old kfv are covered in new kfv
            m_syncTable->set(key, kfvFieldsValues(newKfv));
        }
        else if (ret < 0)
        {
            // Some field exists in old kfv but not in new kfv
            // We have to delete the key and set it again with new kfv
            m_syncTable->del(key);

            // Don't erase the key for m_kfvMap, as we need to set it later.
            del2set = true;
            continue;
        }
        // erase the enetry from kfv map
        m_kfvMap.erase(key);
    }

    if (del2set)
    {
        // We have to make sure m_syncTable buffer (producerStateTable) has been picked up
        // by orchagent, otherwise the set request will be combined with previous del request
        // stale field will be left in appDB


        // TODO: handle it properly.
        while(m_syncTable->count())
        {
            SWSS_LOG_ERROR("Warm-Restart reconciliation: waiting for orchagent to pick up data\n");
            sleep(1);
        }

    }
    // all data remaining in m_kfvMap are new entries or entries which have subset of fields in old entries.
    for (auto &kkfv : m_kfvMap)
    {
        KeyOpFieldsValuesTuple &newKfv = kkfv.second;
        if(kfvOp(newKfv) == DEL_COMMAND)
        {
            // May get an add and a delete while the timer is running for
            // an entry that wasn't in the appdb
            SWSS_LOG_DEBUG("Warm-Restart reconciliation: deleting non-existing entry in appDB%s\n",
                            kfvKey(newKfv).c_str());
        }
        else
        {
            m_syncTable->set(kfvKey(newKfv), kfvFieldsValues(newKfv));
        }
    }
    // Release the map
    m_kfvMap.clear();
    // clear restoreation vector to release memory
    m_restorationVector.clear();
    setState(WarmStart::RECONCILED);
}
/*
 * Save new data from application.
 */
void WarmStartHelper::insertNewDataMap(const std::string &key,
                                        const KeyOpFieldsValuesTuple &kfv)
{
    m_kfvMap[key] = kfv;
}


/*
 * Invoked by warmStartHelper clients during initialization. All interested parties
 * are expected to call this method to upload their associated redisDB state into
 * a temporary buffer, which will eventually serve to resolve any conflict between
 * 'old' and 'new' state.
 */
bool WarmStartHelper::runRestoration()
{
    bool state_available;

    SWSS_LOG_NOTICE("Initiating AppDB restoration process");


    if (buildRestorationVector())
    {
        setState(WarmStart::RESTORED);
        state_available = true;
    }
    else
    {
        setState( WarmStart::RECONCILED);
        state_available = false;
    }

    SWSS_LOG_NOTICE("Completed AppDB restoration process");

    return state_available;
}

bool WarmStartHelper::buildRestorationVector(void)
{

    m_restorationTable.getContent(m_restorationVector);
    if (!m_restorationTable.size())
    {
        SWSS_LOG_NOTICE("Warm-Restart: No records received from AppDB\n");
        return false;
    }
    SWSS_LOG_NOTICE("Warm-Restart: Received %zd records from AppDB\n",
                    restorationVector.size());

    return true;
}

/*
 * Compare two FieldValueTuple vector.
 * return 0 if equal.
 *    case like below is treated as equal:
 *    nh: 10.1.1.1,10.2.2.2, if: eth1,eth2)
 *    nh: 10.2.2.2,10.1.1.1, if: eth2,eth1)
 * return 1 if all fields in vfv2 may be found in vfv1
 * return -1 if there is field in vfv2 not existing in vfv1
 */
int WarmStartHelper::compareFv(const std::vector<FieldValueTuple> &vfv1,
                                const std::vector<FieldValueTuple> &vfv2)
{
    std::unordered_map<std::string, std::string> fvMap;

    if (vfv1.size() < vfv2.size())
    {
        return -1;
    }

    for(auto fv : vfv1)
    {
        fvMap[fvField(fv)] =  fvValue(fv);
    }

    int ret = 0;

    for(auto fv : vfv2)
    {
        if(fvMap.find(fvField(fv)) == fvMap.end())
        {
            return -1;
        }
        if(isValueEqual(fvMap[fvField(fv)], fvValue(fv)) == false)
        {
            // Temporarily assuming all fields in vfv2 may be found in vfv1
            ret = 1;
        }
    }
    return ret;
}

/*
 * Compare two value in FieldValueTuple
 *  cases like below is treated as equal:
 *  "10.1.1.1,10.2.2.2", "10.2.2.2,10.1.1.1"
 *  "eth1,eth2", "eth2,eth1"
 */
bool WarmStartHelper::isValueEqual(const std::string &v1, const std::string &v2)
{
    if (v1.size() != v2.size())
    {
        return false;
    }
    std::vector<std::string> splitValuesV1 = tokenize(v1, ',');
    std::vector<std::string> splitValuesV2 = tokenize(v2, ',');
    if (splitValuesV1.size() != splitValuesV2.size())
    {
        return false;
    }
    std::sort(splitValuesV1.begin(), splitValuesV1.end());
    std::sort(splitValuesV2.begin(), splitValuesV2.end());

    for(auto i=0; i<splitValuesV1.size(); i++)
    {
        if (splitValuesV1[i] != splitValuesV2[i])
        {
            return false;
        }
    }
    return true;
}
