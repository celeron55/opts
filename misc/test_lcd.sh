#!/bin/bash
systemctl stop autosoitin
# Show messages on LCD during update
if   [ -e /dev/ttyUSB0 ]; then
        ARDUINO_SERIAL="/dev/ttyUSB0"
elif [ -e /dev/ttyUSB1 ]; then
        ARDUINO_SERIAL="/dev/ttyUSB1"
else
        ARDUINO_SERIAL="/dev/ttyUSB2"
fi
echo "ARDUINO_SERIAL=$ARDUINO_SERIAL"
stty -F $ARDUINO_SERIAL 9600
exec 3<> $ARDUINO_SERIAL
sleep 2
echo -ne ">SET_TEXT:TEST $(date +%S)\r\n" >&3
sleep 3
exec 3>&-
