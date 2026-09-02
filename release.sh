#!/bin/sh -e
git checkout release
git rm -rf . --quiet
git checkout patches -- .
git checkout master -- *.c *.h cbuild.sh
git checkout manual -- man2ascii.sh gencodemap.sh vi.1
git checkout patch2vi -- patch2vi.c build_patch2vi.sh test_patch2vi.sh deltas.sh
git checkout test -- test.sh
git checkout release -- release.sh CHANGELOG
blurb=$(for b in master manual patches patch2vi test; do
	printf "%-9s %s\n" "$b" "$(git rev-parse --short "$b")"
done)
tmp=$(mktemp)
awk -v blk="$blurb" '
	/^------------------$/ && prev == "" && ++n == 2 {
		print "Source commits:"
		print blk
		print ""
	}
	{ prev = $0; print }
' CHANGELOG > "$tmp"
mv "$tmp" CHANGELOG
rd=$(cat README)
./man2ascii.sh
printf "\n%s\n" "$rd" >> README
git add .
git commit -m "$1"
