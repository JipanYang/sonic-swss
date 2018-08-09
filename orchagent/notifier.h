#include "orch.h"

class Notifier : public Executor {
public:
    Notifier(NotificationConsumer *select, Orch *orch, const string &name)
        : Executor(select, orch, name)
    {
    }

    NotificationConsumer *getNotificationConsumer() const
    {
        return static_cast<NotificationConsumer *>(getSelectable());
    }

    void execute(bool apply=true)
    {
        m_orch->doTask(*getNotificationConsumer());
    }
};
