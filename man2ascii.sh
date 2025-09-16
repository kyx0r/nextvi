#!/bin/sh
export current_date="$(date +"%b %e, %Y")"
export EXINIT="$(printf '%b' 'c .Dd ${current_date}\n.\n:/CODE MAP/:.+4,.+18!./gencodemap.sh:wq')"
eval "EXINIT=\"$EXINIT\""
vi -s ./vi.1
man -T ascii ./vi.1 > README
EXINIT="$(printf '%b' '%s/.\x8(.)/\\1/g:wq')" vi -s ./README
