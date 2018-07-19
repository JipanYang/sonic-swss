from swsscommon import swsscommon
import os
import re
import time
import json

def test_OrchagentWarmRestartReadyCheck(dvs):

    dvs.runcmd("config warm_restart enable swss")
    # hostcfgd not running in VS, create the folder explicitly
    dvs.runcmd("mkdir -p /etc/sonic/warm_restart/swss")

    dvs.runcmd("ifconfig Ethernet0 10.0.0.0/31 up")
    dvs.runcmd("ifconfig Ethernet4 10.0.0.2/31 up")

    dvs.servers[0].runcmd("ifconfig eth0 10.0.0.1/31")
    dvs.servers[0].runcmd("ip route add default via 10.0.0.0")

    dvs.servers[1].runcmd("ifconfig eth0 10.0.0.3/31")
    dvs.servers[1].runcmd("ip route add default via 10.0.0.2")


    db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    ps = swsscommon.ProducerStateTable(db, "ROUTE_TABLE")
    fvs = swsscommon.FieldValuePairs([("nexthop","10.0.0.1"), ("ifname", "Ethernet0")])

    ps.set("2.2.2.0/24", fvs)

    time.sleep(1)
    #
    result =  dvs.runcmd("/usr/bin/orchagent_restart_check")
    assert result == "RESTARTCHECK failed\n"

    # get neighbor and arp entry
    dvs.servers[0].runcmd("ping -c 1 10.0.0.3")

    time.sleep(1)
    result =  dvs.runcmd("/usr/bin/orchagent_restart_check")
    assert result == "RESTARTCHECK succeeded\n"


def test_swss_warm_restore(dvs):

    # syncd warm start with temp view not supported yet
    if dvs.tmpview == True:
        return

    dvs.runcmd("/usr/bin/stop_swss.sh")
    time.sleep(3)
    dvs.runcmd("mv /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b")
    dvs.runcmd("/usr/bin/swss-flushdb")
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

    warmtbl = swsscommon.Table(db, "WARM_RESTART_TABLE")

    keys = warmtbl.getKeys()
    print(keys)

    # restart_count for each process in SWSS should be 1
    for key in ['vlanmgrd', 'portsyncd', 'orchagent', 'neighsyncd']:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restart_count":
                assert fv[1] == "1"
            elif fv[0] == "state_restored":
                assert fv[1] == "true"

def test_swss_port_state_syncup(dvs):
    # syncd warm start with temp view not supported yet
    if dvs.tmpview == True:
        return

    dvs.runcmd("/usr/bin/stop_swss.sh")
    time.sleep(3)
    dvs.runcmd("mv /var/log/swss/sairedis.rec /var/log/swss/sairedis.rec.b")

    # Change port state before swss up again
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
    dvs.runcmd("/usr/bin/swss-flushdb")
    dvs.runcmd("/usr/bin/start_swss.sh")
    time.sleep(10)

    db = swsscommon.DBConnector(0, dvs.redis_sock, 0)

    warmtbl = swsscommon.Table(db, "WARM_RESTART_TABLE")

    # restart_count for each process in SWSS should be 2
    keys = warmtbl.getKeys()
    print(keys)
    for key in keys:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restart_count":
                assert fv[1] == "2"
            elif fv[0] == "state_restored":
                assert fv[1] == "true"

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


def create_entry(tbl, key, pairs):
    fvs = swsscommon.FieldValuePairs(pairs)
    tbl.set(key, fvs)

    # FIXME: better to wait until DB create them
    time.sleep(1)

def create_entry_tbl(db, table, key, pairs):
    tbl = swsscommon.Table(db, table)
    create_entry(tbl, key, pairs)

def del_entry_tbl(db, table, key):
    tbl = swsscommon.Table(db, table)
    tbl._del(key)

def create_entry_pst(db, table, key, pairs):
    tbl = swsscommon.ProducerStateTable(db, table)
    create_entry(tbl, key, pairs)

def how_many_entries_exist(db, table):
    tbl =  swsscommon.Table(db, table)
    return len(tbl.getKeys())

def getCrmCounterValue(dvs, key, counter):

    counters_db = swsscommon.DBConnector(swsscommon.COUNTERS_DB, dvs.redis_sock, 0)
    crm_stats_table = swsscommon.Table(counters_db, 'CRM')

    for k in crm_stats_table.get(key)[1]:
        if k[0] == counter:
            return int(k[1])
    return 0

def test_swss_fdb_syncup_and_crm(dvs):
    # syncd warm start with temp view not supported yet
    if dvs.tmpview == True:
        return

    # Prepare FDB entry before swss stop
    appl_db = swsscommon.DBConnector(swsscommon.APPL_DB, dvs.redis_sock, 0)
    asic_db = swsscommon.DBConnector(swsscommon.ASIC_DB, dvs.redis_sock, 0)
    conf_db = swsscommon.DBConnector(swsscommon.CONFIG_DB, dvs.redis_sock, 0)

    # create a FDB entry in Application DB
    create_entry_pst(
        appl_db,
        "FDB_TABLE", "Vlan2:52-54-00-25-06-E9",
        [
            ("port", "Ethernet12"),
            ("type", "dynamic"),
        ]
    )
    # create vlan
    create_entry_tbl(
        conf_db,
        "VLAN", "Vlan2",
        [
            ("vlanid", "2"),
        ]
    )

    # create vlan member entry in application db. Don't use Ethernet0/4/8 as IP configured on them in previous testing.
    create_entry_tbl(
        conf_db,
        "VLAN_MEMBER", "Vlan2|Ethernet12",
         [
            ("tagging_mode", "untagged"),
         ]
    )
    # check that the FDB entry was inserted into ASIC DB
    assert how_many_entries_exist(asic_db, "ASIC_STATE:SAI_OBJECT_TYPE_FDB_ENTRY") == 1, "The fdb entry wasn't inserted to ASIC"

    dvs.runcmd("crm config polling interval 1")
    time.sleep(2)
    # get counters
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_used')
    assert used_counter == 1

    # Change the polling interval to 20 so we may see the crm counter changes after warm restart
    dvs.runcmd("crm config polling interval 20")

    dvs.runcmd("/usr/bin/stop_swss.sh")

    # delete the FDB entry in AppDB before swss is started again,
    # the orchagent is supposed to sync up the entry from ASIC DB after warm restart
    del_entry_tbl(appl_db, "FDB_TABLE", "Vlan2:52-54-00-25-06-E9")


    time.sleep(1)
    dvs.runcmd("/usr/bin/start_swss.sh")
    time.sleep(10)

    # restart_count for each process in SWSS should be 3
    warmtbl = swsscommon.Table(appl_db, "WARM_START_TABLE")
    keys = warmtbl.getKeys()
    print(keys)
    for key in keys:
        (status, fvs) = warmtbl.get(key)
        assert status == True
        for fv in fvs:
            if fv[0] == "restart_count":
                assert fv[1] == "3"
            elif fv[0] == "state_restored":
                assert fv[1] == "true"

    # get counters for FDB entries, it should be 0
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_used')
    assert used_counter == 0
    dvs.runcmd("crm config polling interval 10")
    time.sleep(20)
     # get counters for FDB entries, it should be 1
    used_counter = getCrmCounterValue(dvs, 'STATS', 'crm_stats_fdb_entry_used')
    assert used_counter == 1

    dvs.runcmd("config warm_restart disable swss")
    # hostcfgd not running in VS, rm the folder explicitly
    dvs.runcmd("rm -f -r /etc/sonic/warm_restart/swss")

