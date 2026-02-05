#!/bin/sh -e

ffor() { IFS="
"; i=0; for p in $1; do eval "$2 \$p $3"; i=$((i + 1)); done }

git checkout "${1:-$(git log --all --format='%H %s' | awk '$2 == "update" && NF == 2 {print $1; exit}')}"
./run_patches.sh 2
git checkout patches
./reindex.sh
ffor "$(ls *.patch)" ./patch2vi.sh
