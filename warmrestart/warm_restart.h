//#pragma once

#ifndef SWSS_WARM_RESTART_H
#define SWSS_WARM_RESTART_H

#include <string>

using namespace swss;

bool isWarmStart();
void checkWarmStart(DBConnector *appl_db, std::string app_name);


#endif
