#!/bin/sh
# Edit embedded deltas in nextvi shell script

if [ ! -x ./patch2vi ]; then
	git checkout patch2vi
	./build_patch2vi.sh build
	git checkout patches
fi

if [ "$2" ]; then
	./patch2vi.sh -i "$1"
else
	./patch2vi.sh -d "$1"
fi
mv "${1}.sh" "$1"
