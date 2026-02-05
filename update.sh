#!/bin/sh -e

gco "$1"
./run_patches.sh 2
./reindex.sh
