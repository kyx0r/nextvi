#!/bin/sh
# Convert current git diff to nextvi shell script
# Usage: gitdiff2vi.sh [flags] [output.sh]
# If output.sh is omitted, writes to stdout

if [ -n "$2" ]; then
	git diff -- ":!$2" | ./patch2vi $1 > "$2"
	chmod +x "$2"
	echo "Generated: $2" >&2
else
	git diff | ./patch2vi $1
fi
