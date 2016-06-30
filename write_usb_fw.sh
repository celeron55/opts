#!/bin/sh
set -eu
MOUNT=$1
cp -r build.sh src common arduino misc $MOUNT/FW/ &&
rm -f $MOUNT/FW/upgraded.txt &&
echo "You can now run: umount \"$MOUNT\"" ||
echo "Failed!"
