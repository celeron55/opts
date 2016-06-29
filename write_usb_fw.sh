#!/bin/sh
set -euv
MOUNT=$1
cp -r build.sh src common arduino misc $MOUNT/FW/ && rm -f $MOUNT/FW/upgraded.txt && umount $MOUNT/
