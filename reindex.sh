for p in *.patch
do
	echo "PATCH: $p"
	patch < $p
	git diff > ./tmp
	patch -R < $p &>/dev/null
	cp tmp $p
	git add $p
	echo "END PATCH: $p"
done
rm tmp *.orig
