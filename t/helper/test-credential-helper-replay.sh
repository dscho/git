cmd=$1
teefile=$cmd-actual.cred
catfile=$cmd-response.cred
rm -f $teefile
while read line;
do
	if test -z "$line"; then
		break;
	fi
	echo "$line" >> $teefile
done
if test "$cmd" = "get"; then
	cat $catfile
fi
