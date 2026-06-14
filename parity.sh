#!/bin/sh
export EXINIT=
printf '%s\n' '? \?p\\\:1q\:p' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '>\\>:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '>\\\>>:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '>\\\\>:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '>\\\\\>>:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '>\\\\\\>:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '? >\\\?:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '? >\\\\?:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '? >\\\\\?:reg' | ./vi -em vi.c
printf "\n\n"
printf '%s\n' '? >\\\\\\?:reg' | ./vi -em vi.c
