#!/bin/bash
# start-clsrv.sh TREE(old|new) SRVPORT TCPFLAG("" or --enabletcp) NCLIENTS
# One server (no --directoryaddress: registers nowhere) plus NCLIENTS headless clients of the same tree,
# started in batches of 8 with 2 s gaps. Client i uses ini34/c<i>.ini (16-char name, 20-char city), base port 50000+100*i.
TREE=$1; SP=$2; TCPFLAG=$3; N=$4
BIN=/home/user/jamulus-$TREE/Jamulus
LOGS=/home/user/harness/logs/clsrv-$TREE-$SP; rm -rf $LOGS; mkdir -p $LOGS
echo "clsrv: tree=$TREE sha=$(git -C /home/user/jamulus-$TREE rev-parse HEAD) port=$SP tcp='$TCPFLAG' clients=$N"
echo "server cmd: $BIN -s -n -p $SP $TCPFLAG -u 50" | tee $LOGS/cmd.txt
$BIN -s -n -p $SP $TCPFLAG -u 50 > $LOGS/server.log 2>&1 &
echo $! > $LOGS/server.pid
sleep 1
for i in $(seq 1 $N); do
  echo "client $i cmd: $BIN -n -c 127.0.0.1:$SP -i /home/user/harness/ini34/c$i.ini --clientname hc$TREE$i -p $((50000+100*i))" >> $LOGS/cmd.txt
  $BIN -n -c 127.0.0.1:$SP -i /home/user/harness/ini34/c$i.ini --clientname hc$TREE$i -p $((50000+100*i)) > $LOGS/client$i.log 2>&1 &
  echo "$i $!" >> $LOGS/clients.pid
  [ $((i % 8)) = 0 ] && sleep 2
done
sleep 5
echo "server log connected lines (last):"; grep 'connected (' $LOGS/server.log | tail -1; grep -i tcp $LOGS/server.log | head -3
