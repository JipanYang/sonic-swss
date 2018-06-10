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

def test_swss_port_state_syncup(dvs):
    # syncd warm start with temp view not supported yet
    if dvs.tmpview == True:
        return

    dvs.runcmd("/usr/bin/stop_swss.sh")
    time.sleep(3)
    dvs.runcmd("mv /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b")

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up")
    dvs.runcmd("ifconfig Ethernet8 10.0.0.4/31 up")

    dvs.runcmd("arp -s 10.0.0.1 00:00:00:00:00:01")
    dvs.runcmd("arp -s 10.0.0.3 00:00:00:00:00:02")
    dvs.runcmd("arp -s 10.0.0.5 00:00:00:00:00:03")

    dvs.servers[0].runcmd("ip link set down dev eth0") == 0
    dvs.servers[1].runcmd("ip link set down dev eth0") == 0
    dvs.servers[2].runcmd("ip link set down dev eth0") == 0
    dvs.servers[2].runcmd("ip link set up dev eth0") == 0

    time.sleep(1)
    dvs.runcmd("/usr/bin/start_swss.sh")
    time.sleep(10)

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

    warmtbl = swsscommon.Table(db, "WARM_START_TABLE")

    keys = warmtbl.getKeys()
    print(keys)

    (status, fvs) = warmtbl.get("vlanmgrd")
    assert status == True
    for fv in fvs:
        if fv[0] == "restart_count":
            assert fv[1] == "2"
        else:
            assert False

    (status, fvs) = warmtbl.get("portsyncd")
    assert status == True
    for fv in fvs:
        if fv[0] == "restart_count":
            assert fv[1] == "2"
        else:
            assert False

    (status, fvs) = warmtbl.get("orchagent")
    assert status == True
    for fv in fvs:
        if fv[0] == "restart_count":
            assert fv[1] == "2"
        else:
            assert False

    tbl = swsscommon.Table(db, "PORT_TABLE")

    for i in [0, 1, 2]:
        (status, fvs) = tbl.get("Ethernet%d" % (i * 4))
        assert status == True

        oper_status = "unknown"

        for v in fvs:
            if v[0] == "oper_status":
                oper_status = v[1]
                break
        if i == 2:
            assert oper_status == "up"
        else:
            assert oper_status == "down"

