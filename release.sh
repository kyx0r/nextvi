#!/bin/sh -e
git checkout patches
patches=$(ls *.patch)
git checkout release
git checkout master *.c *.h cbuild.sh
git checkout manual man2ascii.sh gencodemap.sh vi.1
git checkout patches $patches README
rd=$(cat README)
./man2ascii.sh
printf "\n%s\n" "$rd" >> README
git add .
git commit -m "$1"
