#!/bin/sh -e

git checkout "${1:-$(git log --all --format='%H %s' | awk '$2 == "update" && NF == 2 {print $1; exit}')}"
./run_patches.sh 2
./reindex.sh
