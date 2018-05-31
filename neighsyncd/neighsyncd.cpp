#include <iostream>
#include "logger.h"
#include "select.h"
#include "netdispatcher.h"
#include "netlink.h"
#include "neighsyncd/neighsync.h"
#include <time.h>

using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    Logger::linkToDbNative("neighsyncd");

    DBConnector appDb(APPL_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisPipeline pipelineAppDB(&appDb);
    DBConnector confDb(CONFIG_DB, DBConnector::DEFAULT_UNIXSOCKET, 0);
    RedisPipeline pipelineConfDB(&confDb);

    NeighSync sync(&pipelineAppDB, &pipelineConfDB);

    NetDispatcher::getInstance().registerMessageHandler(RTM_NEWNEIGH, &sync);
    NetDispatcher::getInstance().registerMessageHandler(RTM_DELNEIGH, &sync);

    while (1)
    {
        try
        {
            NetLink netlink;
            Select s;

            netlink.registerGroup(RTNLGRP_NEIGH);
            cout << "Listens to neigh messages..." << endl;
            netlink.dumpRequest(RTM_GETNEIGH);

            s.addSelectable(&netlink);
	    sync.readTableToMap();
            while (true)
            {
                Selectable *temps;
                s.select(&temps, SELECT_TIMEOUT);
		if (sync.checkReconcile())
		{
		    sync.reconcile(sync.get_ps_table());
		}
            }
        }
        catch (const std::exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in deamon" << endl;
            return 0;
        }
    }

    return 1;
}
