#!/bin/sh -e
git checkout release
git rm -rf . --quiet
git checkout patches -- .
git checkout master -- *.c *.h cbuild.sh
git checkout manual -- man2ascii.sh gencodemap.sh vi.1
git checkout patch2vi -- patch2vi.c build.sh
git checkout test -- test.sh
git checkout release -- release.sh
rd=$(cat README)
./man2ascii.sh
printf "\n%s\n" "$rd" >> README
git add .
git commit -m "$1"
