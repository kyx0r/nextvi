#!/bin/sh -e
for p in *.c *.h; do
	sed -i "s/\<unsigned int\> /u64 /g" "$p"
	sed -i "s/(unsigned int) /u64 /g" "$p"
	sed -i "s/(unsigned int\*) /u64 /g" "$p"
	sed -i "s/\<int\> /s64 /g" "$p"
	sed -i "s/(int)/(s64)/g" "$p"
	sed -i "s/(int\*)/(s64*)/g" "$p"
	sed -i "s/s64 signo/int signo/g" "$p"
	sed -i "s/s64 pipefds/int pipefds/g" "$p"
	sed -i "s/s64 main(s64/int main(int/g" "$p"
	sed -i "s/abs(/labs(/g" "$p"
	sed -i "s/%d/%ld/g" "$p"
	sed -i "s/%+d/%+ld/g" "$p"
	sed -i "s/%04x/%04lx/g" "$p"
done
sed -i "1i#include <stdint.h>\ntypedef uint64_t u64;\ntypedef int64_t s64;\n" vi.c
