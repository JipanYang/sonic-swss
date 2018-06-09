from swsscommon import swsscommon
import os
import re
import time
import json

def test_swss_warm_restore(dvs):

    # syncd warm start with temp view not supported yet
    if dvs.tmpview == True:
        return

    dvs.runcmd("/usr/bin/stop_swss.sh")
    time.sleep(3)
    dvs.runcmd("mv /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b")
    dvs.runcmd("/usr/bin/start_swss.sh")
    time.sleep(10)

    # No create/set/remove operations should be passed down to syncd for swss restore
    num = dvs.runcmd(['sh', '-c', 'grep \|c\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'
    num = dvs.runcmd(['sh', '-c', 'grep \|s\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'
    num = dvs.runcmd(['sh', '-c', 'grep \|r\| /var/log/swss/sairedis.rec | wc -l'])
    assert num == '0\n'

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

    warmtbl = swsscommon.Table(db, "WARM_START_TABLE")

    keys = warmtbl.getKeys()
    print(keys)

    (status, fvs) = warmtbl.get("vlanmgrd")
    assert status == True
    for fv in fvs:
        if fv[0] == "restart_count":
            assert fv[1] == "1"
        else:
            assert False

    (status, fvs) = warmtbl.get("portsyncd")
    assert status == True
    for fv in fvs:
        if fv[0] == "restart_count":
            assert fv[1] == "1"
        else:
            assert False

    (status, fvs) = warmtbl.get("orchagent")
    assert status == True
    for fv in fvs:
        if fv[0] == "restart_count":
            assert fv[1] == "1"
        else:
            assert False


