#!/bin/sh
EXINIT=$'se noled:/fprintf/:&J0ci(abc\x1bci)no\x1b\:&ci)test\n\x1bci)wooaaa\x1bci(next:se led:.-1,.+2p:q!'
rm ./file
./vi -e vi.c
cat ./file
