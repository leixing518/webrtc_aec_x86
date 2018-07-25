#!/bin/sh
make clean
make

g++ test/test.cpp -DWEBRTC_APM_DEBUG_DUMP=0 -DWEBRTC_LINUX -DWEBRTC_POSIX -std=c++11 -Iinc -L. -laec -lstdc++ -lm -lpthread -o testaec
