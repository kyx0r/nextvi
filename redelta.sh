#!/bin/sh

if [ -z "$1" ]; then
	echo "Usage: $0 input.sh" >&2
	exit 1
fi

git add .
git diff --staged > /tmp/tmp.patch
sed '/^=== PATCH2VI PATCH ===$/q' "$1" > /tmp/tmp.sh
mv /tmp/tmp.sh "$1"
cat /tmp/tmp.patch >> "$1"
./patch2vi -d "$1" > "_$1"
mv "_$1" "$1"
chmod +x "$1"
echo "Generated: $1" >&2
git reset
