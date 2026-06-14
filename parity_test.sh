#!/bin/sh
unset EXINIT
printf '%s\n' '? \?p\\\:1q\:p BAD:q!' | ./vi -em vi.c
printf '%s\n' '>\\>:reg:q!' | ./vi -em vi.c
printf '%s\n' '>\\\>>:reg:q!' | ./vi -em vi.c
printf '%s\n' '>\\\\>:reg:q!' | ./vi -em vi.c
printf '%s\n' '>\\\\\>>:reg:q!' | ./vi -em vi.c
printf '%s\n' '>\\\\\\>:reg:q!' | ./vi -em vi.c
printf '%s\n' '? >\\\?:reg:q!' | ./vi -em vi.c
printf '%s\n' '? >\\\\?:reg:q!' | ./vi -em vi.c
printf '%s\n' '? >\\\\\?:reg:q!' | ./vi -em vi.c
printf '%s\n' '? >\\\\\\?:reg:q!' | ./vi -em vi.c
printf '%s\n' '? \?0\?\:0\?:q!' | ./vi -em vi.c
printf '%s\n' '? \?0\\?\\:0\\?:q!' | ./vi -em vi.c
printf '%s\n' '? \?0\\\?\\\:0\\\?:q!' | ./vi -em vi.c
printf '%s\n' '? \?0\\\\?\\\\:0\\\\?:q!' | ./vi -em vi.c
printf '%s\n' '? \? \\\? 0\\\\\\\?\\\\\\\:0\\\\\\\?:q!' | ./vi -em vi.c
printf '%s\n' '? \? \\\? p \\\\\\\?' | ./vi -em vi.c
