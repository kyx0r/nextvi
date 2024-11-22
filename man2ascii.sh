#!/bin/sh
current_date=$(date +"%b %e, %Y")
EXINIT=$'c .Dd ${current_date}\n.\n:/CODE MAP/:.+4,.+18!./gencodemap.sh:wq'
eval "EXINIT=\"$EXINIT\""
vi -s ./vi.1
man -T ascii ./vi.1|col -b > README
