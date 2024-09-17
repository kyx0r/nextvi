#!/bin/sh -e
for p in *.patch
do
	printf "%s\n" "PATCH: $p"
	patch --merge=diff3 < $p
	[ "$1" = "1" ] && ./cbuild.sh build
	git diff > /tmp/tmp.patch
	patch -R < $p &>/dev/null
	cp /tmp/tmp.patch $p
	git add $p
	printf "\n"
	[ "$1" = "2" ] && git reset --hard
done
rm -f tmp *.orig *.rej
