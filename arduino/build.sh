#!/bin/sh
echo -n "static const char *VERSION_STRING = \"" > version.h
echo -n $(md5sum src/sketch.ino | cut -d' ' -f1) >> version.h
echo "\";" >> version.h
ano build -m nano --cpu atmega328
