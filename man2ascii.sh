#!/bin/sh
EXINIT='/CODE MAP/|.+2,.+16!./gencodemap.sh|wq'
vi -s ./vi.1
man ./vi.1|col -b > README
