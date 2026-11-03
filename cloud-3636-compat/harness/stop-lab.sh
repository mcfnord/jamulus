#!/bin/bash
pkill -f '/home/user/jamulus-(old|new)/Jamulus -s' ; sleep 1; pgrep -af 'Jamulus -s' | wc -l
