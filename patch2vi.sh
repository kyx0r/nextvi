#!/bin/sh
# Convert .patch file to nextvi shell script
# Usage: patch2vi.sh input.patch [output.sh]

if [ -z "$1" ]; then
	echo "Usage: $0 input.patch [output.sh]" >&2
	exit 1
fi

input="$2"
if [ -n "$3" ]; then
	output="$3"
else
	# Replace .patch extension with .sh, or append .sh if no .patch ext
	case "$input" in
		*.patch) output="${input%.patch}.sh" ;;
		*) output="${input}.sh" ;;
	esac
fi

./patch2vi "$1" < "$input" > "$output"
chmod +x "$output"
echo "Generated: $output"
