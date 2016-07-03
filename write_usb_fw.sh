#!/bin/sh
set -eu
MOUNT=$1
cp -r build.sh src common arduino misc "$MOUNT"/FW/ &&
echo "rm -f player; cp opts player" >> "$MOUNT"/FW/build.sh
rm -f "$MOUNT"/FW/upgraded.txt &&
echo "You can now run: umount \"$MOUNT\"" ||
echo "Failed!"
