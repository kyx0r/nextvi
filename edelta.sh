#!/bin/sh
# Edit embedded deltas in nextvi shell script

if [ "$2" ]; then
	./patch2vi.sh -i "$1"
else
	./patch2vi.sh -d "$1"
fi
mv "${1}.sh" "$1"
