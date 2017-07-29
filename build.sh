#!/bin/sh
g++ -o opts src/main.cpp src/c55_getopt.cpp src/file_watch.cpp src/filesys.cpp src/arduino_firmware.cpp src/arduino_global.cpp src/media_scan.cpp src/mpv_control.cpp src/mkdir_p.cpp src/ui_output_queue.cpp `pkg-config --libs --cflags mpv` --std=c++0x -Wall -Wno-unused-function -g
