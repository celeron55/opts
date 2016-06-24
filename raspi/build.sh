#!/bin/sh
g++ -o player src/main.cpp src/c55_getopt.cpp `pkg-config --libs --cflags mpv` --std=c++0x
