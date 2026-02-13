#!/bin/sh
# Convert current git diff to nextvi shell script
# Usage: gitdiff2vi.sh [output.sh]

output="${2:-tmp.sh}"
git diff -- ":!$output" | ./patch2vi "$1" > "$output"
chmod +x "$output"
echo "Generated: $output"
