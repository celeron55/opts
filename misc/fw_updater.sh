#!/bin/sh

MOUNTPOINT=/tmp/__autosoitin_mnt
INSTALLPOINT=/home/pi/dev/autosoitin

echo "Autosoitin fw_updater.sh started"

while true; do
	sleep 5
	if [ -d "$MOUNTPOINT"/FW ]; then
		if [ -e "$MOUNTPOINT"/FW/upgraded.txt ]; then
			continue
		fi

		echo "Upgrading autosoitin software"

		systemctl stop autosoitin

		echo -ne ">SET_TEXT:SW UP\r\n" > /dev/ttyUSB0
		echo -ne ">SET_TEXT:SW UP\r\n" > /dev/ttyUSB1
		echo -ne ">SET_TEXT:SW UP\r\n" > /dev/ttyUSB2

		mkdir -p "$INSTALLPOINT"
		cp -r "$MOUNTPOINT"/FW/* "$INSTALLPOINT"/

		cd "$INSTALLPOINT"
		./build.sh

		systemctl start autosoitin

		mount -o remount,rw /tmp/__autosoitin_mnt/
		date > "$MOUNTPOINT"/FW/upgraded.txt
		mount -o remount,ro /tmp/__autosoitin_mnt/

		echo "Autosoitin software upgraded"
	fi
done
