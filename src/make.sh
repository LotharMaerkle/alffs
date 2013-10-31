#!/bin/bash
set -e

rm -f alf

gcc -Wall -g -O0 -DHASH_DEBUG=1 -D_FILE_OFFSET_BITS=64 alf.c -o alf `pkg-config fuse json --cflags --libs` `curl-config --cflags --libs` `pkg-config openssl --libs` -std=c99 -lpthread 


