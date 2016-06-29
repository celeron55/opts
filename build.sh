#!/bin/sh
g++ -o opts src/main.cpp src/c55_getopt.cpp src/file_watch.cpp src/filesys.cpp src/arduino_firmware.cpp src/mkdir_p.cpp `pkg-config --libs --cflags mpv` --std=c++0x -Wall -Wno-unused-function -g &&
rm -f player && cp opts player
