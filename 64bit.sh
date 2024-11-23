#!/bin/sh -e
for p in *.c *.h; do
	EXINIT="%s/(\<unsigned int\> )/u64 /g"
	EXINIT="${EXINIT}:%s/\(unsigned int\)/(u64)/g"
	EXINIT="${EXINIT}:%s/\(unsigned int\*\)/(u64*) /g"
	EXINIT="${EXINIT}:%s/(\<int\> )/s64 /g"
	EXINIT="${EXINIT}:%s/\(int\)/(s64)/g"
	EXINIT="${EXINIT}:%s/\(int\*\)/(s64*)/g"
	EXINIT="${EXINIT}:%s/\<int,\>/s64,/g"
	EXINIT="${EXINIT}:%s/abs\(/labs(/g"
	EXINIT="${EXINIT}:%s/\%d/\%ld/g"
	EXINIT="${EXINIT}:%s/\%\+d/\%+ld/g"
	EXINIT="${EXINIT}:%s/\%08x/\%08lx/g"
	EXINIT="${EXINIT}:%s/s64 signo/int signo/g"
	EXINIT="${EXINIT}:%s/s64 pipefds/int pipefds/g"
	EXINIT="${EXINIT}:%s/s64 main\(s64/int main(int/g"
	EXINIT="${EXINIT}:wq"
	vi -sm "$p"
done
EXINIT=$'1:i \#include <stdint.h>\ntypedef uint64_t u64;\ntypedef int64_t s64;\n.\n:wq' vi -sm vi.h
