#pragma once

#include "orch.h"

class SwitchOrch : public Orch
{
public:
    SwitchOrch(DBConnector *db, string tableName);
    bool checkRestartReady() { return checkRestartReadyState; }
    bool warmRestartCheckReply(const vector<string> &ts);

private:
    void doTask(Consumer &consumer);

    NotificationConsumer* m_restartCheckNotificationConsumer;
    void doTask(NotificationConsumer& consumer);
    // Whether to check readiness of warm restart
    bool checkRestartReadyState = false;
    DBConnector *m_db;
};
