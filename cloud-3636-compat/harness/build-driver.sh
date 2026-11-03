#!/bin/bash
# build-driver.sh TREE_DIR BUILD_LOG OUT TREE_DEFINE
set -e
TREE=$1; LOG=$2; OUT=$3; DEF=$4
CXXFLAGS=$(grep -m1 '^g++ -c' "$LOG" | sed 's/^g++ -c //' | sed -E 's/ -o release\/[^ ]+ src\/.*$//')
LIBS=$(grep -m1 '^g++ -Wl' "$LOG" | sed -E 's/.* release\/[^ ]+\.o //')
cd "$TREE"
OBJS=$(ls release/*.o | grep -v '/main\.o$')
g++ $CXXFLAGS -DTREE_NAME="\"$DEF\"" -D$DEF -o /home/user/harness/$OUT.o -c /home/user/harness/cldrv.cpp
g++ -o /home/user/harness/$OUT /home/user/harness/$OUT.o $OBJS $LIBS
echo "built /home/user/harness/$OUT from $TREE ($(git -C $TREE rev-parse HEAD))"
