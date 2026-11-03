#!/bin/bash
pkill -f '/home/user/jamulus-(old|new)/Jamulus -n -c' ; pkill -f '/home/user/jamulus-(old|new)/Jamulus -s'; sleep 2; pgrep -f 'Jamulus -' | wc -l
