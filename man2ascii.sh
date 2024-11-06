#!/bin/sh
EXINIT='/CODE MAP/|.+4,.+18!./gencodemap.sh|wq'
vi -s ./vi.1
man -T ascii ./vi.1|col -b > README
