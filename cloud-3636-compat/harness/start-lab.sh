#!/bin/bash
# start-lab.sh TREE(old|new) DIRPORT TCPFLAG("" or --enabletcp) NSERVERS
# Starts one local directory and NSERVERS local servers registered with it, all on 127.0.0.1.
TREE=$1; DIRPORT=$2; TCPFLAG=$3; N=${4:-50}
BIN=/home/user/jamulus-$TREE/Jamulus
LOGS=/home/user/harness/logs/lab-$TREE-$DIRPORT; mkdir -p $LOGS
echo "lab: tree=$TREE sha=$(git -C /home/user/jamulus-$TREE rev-parse HEAD) dirport=$DIRPORT tcp='$TCPFLAG' servers=$N"
# directory: a server whose --directoryaddress is "localhost" acts as the directory (serverlist.cpp SetIsDirectory)
$BIN -s -n -p $DIRPORT -e localhost $TCPFLAG --serverinfo "LocalDirectory$DIRPORT;Directory City;0" -u 10 > $LOGS/directory.log 2>&1 &
echo $! > $LOGS/directory.pid
sleep 1
for i in $(seq 1 $N); do
  P=$((DIRPORT + i))
  NAME=$(printf 'Srv%02d-LongServerNm' $i)    # 20 chars = MAX_LEN_SERVER_NAME
  CITY=$(printf 'City%02d-LongCityName' $i)   # 20 chars = MAX_LEN_SERVER_CITY
  # the NEW-tree server also gets TCPFLAG so its CLM_CONN_CLIENTS_LIST path offers TCP; OLD ignores (flag absent)
  $BIN -s -n -p $P -e 127.0.0.1:$DIRPORT $TCPFLAG --serverinfo "$NAME;$CITY;$((i % 200))" -u 150 > $LOGS/server$i.log 2>&1 &
  echo $! >> $LOGS/servers.pid
done
sleep 4
echo "registered (directory log):"; grep -c 'registered\|Registered' $LOGS/directory.log; grep -i 'tcp' $LOGS/directory.log
