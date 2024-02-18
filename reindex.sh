#!/bin/sh
for p in *.patch
do
	printf "%s\n" "PATCH: $p"
	patch < $p
	git diff > ./tmp
	patch -R < $p &>/dev/null
	cp tmp $p
	git add $p
	printf "\n"
done
rm -f tmp *.orig *.rej
