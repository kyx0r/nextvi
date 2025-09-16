#!/bin/sh -e
git checkout master *.c *.h cbuild.sh
git checkout manual man2ascii.sh gencodemap.sh vi.1
git checkout patches *.patch README
rd=$(cat README)
./man2ascii.sh
printf "\n%s\n" "$rd" >> README
git add .
git commit -m "$1"
