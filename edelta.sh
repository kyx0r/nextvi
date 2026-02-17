#!/bin/sh
# Edit embedded deltas in nextvi shell script

./patch2vi.sh -d "$1"
mv "${1}.sh" "$1"
