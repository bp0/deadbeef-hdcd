#!/bin/bash

gcc -fPIC -g -c dsp_hdcd.c
gcc -shared -O2 -o ddb_hdcd.so dsp_hdcd.o -lc -lm -lhdcd
rm dsp_hdcd.o

