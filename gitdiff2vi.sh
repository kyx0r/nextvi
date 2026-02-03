#!/bin/sh
# Convert current git diff to nextvi shell script
# Usage: gitdiff2vi.sh [output.sh]

output="${1:-tmp.sh}"
git diff -- ":!$output" | ./patch2vi > "$output"
chmod +x "$output"
echo "Generated: $output"
