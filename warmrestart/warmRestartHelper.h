#ifndef __WARMRESTART_HELPER__
#define __WARMRESTART_HELPER__


#include <vector>
#include <map>
#include <unordered_map>

#include "dbconnector.h"
#include "producerstatetable.h"
#include "netmsg.h"
#include "table.h"
#include "tokenize.h"
#include "warm_restart.h"


namespace swss {



class WarmStartHelper {
  public:

    WarmStartHelper(RedisPipeline      *pipeline,
                    ProducerStateTable *syncTable,
                    const std::string  &syncTableName,
                    const std::string  &dockerName,
                    const std::string  &appName);

    ~WarmStartHelper();

    void setState(WarmStart::WarmStartState state);

    WarmStart::WarmStartState getState(void) const;

    bool isEnabled(void);

    bool isReconciled(void) const;

    bool inProgress(void) const;

    uint32_t getRestartTimer(void) const;

    bool runRestoration(void);
    void reconcile(void);
    void insertNewDataMap(const std::string &key,
                          const KeyOpFieldsValuesTuple &kfv);

  private:
    Table                     m_restorationTable;  // redis table to import current-state from
    ProducerStateTable       *m_syncTable;         // producer-table to sync/push state to
    WarmStart::WarmStartState m_state;             // cached value of warmStart's FSM state
    bool                      m_enabled;           // warm-reboot enabled/disabled status
    std::string               m_syncTableName;     // producer-table-name to sync/push state to
    std::string               m_dockName;          // sonic-docker requesting warmStart services
    std::string               m_appName;           // sonic-app requesting warmStart services

    std::vector<KeyOpFieldsValuesTuple> m_restorationVector;
    std::unordered_map<std::string, KeyOpFieldsValuesTuple &kfv> m_kfvMap;

    bool buildRestorationVector(void);
    bool isValueEqual(const std::string &v1, const std::string &v2);
    int compareFv(const std::vector<FieldValueTuple> &vfv1,
                   const std::vector<FieldValueTuple> &vfv2);
};


}

#endif
