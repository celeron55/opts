#!/bin/bash

MOUNTPOINT=/tmp/__autosoitin_mnt
INSTALLPOINT=/home/pi/dev/autosoitin

echo "Autosoitin fw_updater.sh started"

while true; do
	sleep 5
	if [ ! -d "$MOUNTPOINT"/FW ]; then
		# Ehm... maybe this helps avoid "bricking" the thing
		mkdir -p "$MOUNTPOINT"
		mount /dev/sda1 "$MOUNTPOINT" -o ro -t vfat 1>/dev/null 2>/dev/null ||
		mount /dev/sdb1 "$MOUNTPOINT" -o ro -t vfat 1>/dev/null 2>/dev/null ||
		mount /dev/sdc1 "$MOUNTPOINT" -o ro -t vfat 1>/dev/null 2>/dev/null ||
		mount /dev/sdd1 "$MOUNTPOINT" -o ro -t vfat 1>/dev/null 2>/dev/null ||
		mount /dev/sde1 "$MOUNTPOINT" -o ro -t vfat 1>/dev/null 2>/dev/null
	fi
	if [ -d "$MOUNTPOINT"/FW ]; then
		if [ -e "$MOUNTPOINT"/FW/upgraded.txt ]; then
			continue
		fi

		echo "Upgrading autosoitin software"

		systemctl stop autosoitin

		# Show messages on LCD during update
		if   [ -e /dev/ttyUSB0 ]; then
			ARDUINO_SERIAL="/dev/ttyUSB0"
		elif [ -e /dev/ttyUSB1 ]; then
			ARDUINO_SERIAL="/dev/ttyUSB1"
		else
			ARDUINO_SERIAL="/dev/ttyUSB2"
		fi
		# This stuff requires bash
		exec 3<> $ARDUINO_SERIAL
		stty -F $ARDUINO_SERIAL 9600
		sleep 2
		echo -ne ">SET_TEXT:SW UP\r\n" >&3

		# Avoid overwriting old executable with an x86 one
		cp "$INSTALLPOINT"/player "$INSTALLPOINT"/player.saved

		mkdir -p "$INSTALLPOINT"
		cp -r "$MOUNTPOINT"/FW/* "$INSTALLPOINT"/

		# Avoid overwriting old executable with an x86 one
		cp "$INSTALLPOINT"/player.saved "$INSTALLPOINT"/player

		echo -ne ">SET_TEXT:BUILD\r\n" >&3

		cd "$INSTALLPOINT"
		./build.sh

		echo -ne ">SET_TEXT:WRITE\r\n" >&3

		mount -o remount,rw /tmp/__autosoitin_mnt/
		date > "$MOUNTPOINT"/FW/upgraded.txt
		mount -o remount,ro /tmp/__autosoitin_mnt/

		echo "Autosoitin software upgraded"

		echo -ne ">SET_TEXT:UPGRADED\r\n" >&3

		sleep 1

		# Close fd 3 ($ARDUINO_SERIAL)
		exec 3>&-

		systemctl start autosoitin
	fi
done
