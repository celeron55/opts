#!/bin/sh
ver=$(date +%Y%m%d.%H%M%S)
rm -rf "/tmp/opts-$ver"
mkdir "/tmp/opts-$ver"
mkdir "/tmp/opts-$ver/src"
cp src/*.cpp "/tmp/opts-$ver/src"
cp src/*.hpp "/tmp/opts-$ver/src"
cp src/*.h "/tmp/opts-$ver/src"
mkdir "/tmp/opts-$ver/common"
cp common/*.hpp "/tmp/opts-$ver/common"
cp build.sh "/tmp/opts-$ver/"
cp manual.txt "/tmp/opts-$ver/"
cd /tmp
tar czf "opts-$ver.tar.gz" "opts-$ver"
