#!/bin/sh
set -euv
MOUNT=$1
cp -r * $MOUNT/FW/ && rm -f $MOUNT/FW/upgraded.txt && umount $MOUNT/
