#!/bin/sh
# Convert current git diff to nextvi shell script
# Usage: gitdiff2vi.sh [flags] [output.sh]
# If output.sh is omitted, writes to stdout

if [ -n "$2" ]; then
	if [ "$1" = "-d" ]; then
		git diff -- ":!$2" > /tmp/tmp.patch
		sed '/^=== PATCH2VI PATCH ===$/q' "$2" > /tmp/tmp.sh
		mv /tmp/tmp.sh "$2"
		cat /tmp/tmp.patch >> "$2"
		./patch2vi $1 "$2" > "_$2"
		mv "_$2" "$2"
	else
		git diff -- ":!$2" | ./patch2vi $1 > "$2"
	fi
	chmod +x "$2"
	echo "Generated: $2" >&2
else
	git diff | ./patch2vi $1
fi
