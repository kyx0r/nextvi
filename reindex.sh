for p in *.patch
do
	patch < $p
	git diff > ./tmp
	patch -R < $p
	cp tmp $p
	git add $p
done
rm tmp *.orig
