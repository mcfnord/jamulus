#!/bin/bash
# run-cell.sh CELLNAME CLIENTTREE(old|new) MODE(serverlist|clientlist) TARGET(host:port) CLIENTPORT TIMEOUT_S DROP(yes|no)
# DROP=yes: drop every UDP datagram to CLIENTPORT whose IP total length exceeds 1500 bytes (what a 1500-MTU path
#           loses when IP fragments are dropped). iptables -f cannot be used on this host: nf_conntrack reassembles
#           fragments before INPUT (verified: rule matched 0 pkts while tcpdump showed fragments on lo).
set -u
CELL=$1; TREE=$2; MODE=$3; TARGET=$4; CP=$5; TMO=$6; DROP=$7; TCPFAIL=${8:-none}   # TCPFAIL: none|reject|drop (applies to TCP dport of TARGET)
OUT=/home/user/harness/logs/cells/$CELL; mkdir -p $OUT
TPORT=${TARGET##*:}
case $TCPFAIL in
  reject) TRULE="INPUT -i lo -p tcp --dport $TPORT --syn -j REJECT --reject-with tcp-reset"; iptables -I $TRULE;;
  drop)   TRULE="INPUT -i lo -p tcp --dport $TPORT --syn -j DROP"; iptables -I $TRULE;;
esac
[ "$TCPFAIL" != none ] && echo "tcp rule: iptables -I $TRULE" | tee -a $OUT/cmd.txt
tcpdump -i lo -n -tttt -l "port $TPORT or port $CP" > $OUT/tcpdump.txt 2>/dev/null &
TD=$!
sleep 0.5
echo "cell=$CELL client=$TREE ($(git -C /home/user/jamulus-$TREE rev-parse HEAD)) mode=$MODE target=$TARGET clientport=$CP drop=$DROP" | tee $OUT/cmd.txt
echo "cmd: /home/user/harness/cldrv-$TREE $MODE $TARGET $CP $TMO" | tee -a $OUT/cmd.txt
/home/user/harness/cldrv-$TREE $MODE $TARGET $CP $TMO 1500 > $OUT/client.log 2>&1 &
DRV=$!
sleep 0.7
REALPORT=$(ss -Hlunp | grep "pid=$DRV," | sed -E 's/.*:([0-9]+) .*/\1/' | head -1)
echo "client real UDP port: $REALPORT" | tee -a $OUT/cmd.txt
RULE="INPUT -i lo -p udp --dport $REALPORT -m length --length 1501:65535 -j DROP"
[ "$DROP" = yes ] && { iptables -I $RULE; echo "rule: iptables -I $RULE" | tee -a $OUT/cmd.txt; }
wait $DRV
sleep 0.5
kill $TD; wait $TD 2>/dev/null
[ "$DROP" = yes ] && { iptables -L INPUT -n -v | grep "dpt:$REALPORT" | tee -a $OUT/cmd.txt; iptables -D $RULE; }
[ "$TCPFAIL" != none ] && { iptables -L INPUT -n -v | grep "dpt:$TPORT" | tee -a $OUT/cmd.txt; iptables -D $TRULE; }
CP=$REALPORT
echo "--- client.log (events) ---"; grep -v '^\[.*QT: .*XRun' $OUT/client.log | grep 'EVENT\|TRANSPORT\|ACTION\|TIMER\|SEND\|END\|QT:' 
echo "--- tcpdump: UDP datagram sizes from target, TCP SYNs ---"
grep -E "UDP, length" $OUT/tcpdump.txt | grep "127.0.0.1.$TPORT > 127.0.0.1.$CP" | awk '{print $NF}' | sort -n | uniq -c | sort -k2 -n | tail -5
grep -E "Flags \[S\]" $OUT/tcpdump.txt | head -5
