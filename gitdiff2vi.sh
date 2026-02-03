#!/bin/sh
# Convert current git diff to nextvi shell script
# Usage: gitdiff2vi.sh [output.sh]

output="${1:-tmp.sh}"
git diff | ./patch2vi > "$output"
echo "Generated: $output"
