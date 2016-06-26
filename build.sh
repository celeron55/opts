#!/bin/sh
g++ -o player src/main.cpp src/c55_getopt.cpp src/file_watch.cpp src/filesys.cpp src/arduino_firmware.cpp `pkg-config --libs --cflags mpv` --std=c++0x -Wall -Wno-unused-function
