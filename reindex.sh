#!/bin/sh -e
for p in *.patch
do
	printf "%s\n" "PATCH: $p"
	patch --merge=diff3 < $p
	[ ! -z "$1" ] && ./cbuild.sh build
	git diff > /tmp/tmp.patch
	patch -R < $p &>/dev/null
	cp /tmp/tmp.patch $p
	git add $p
	printf "\n"
done
rm -f tmp *.orig *.rej
