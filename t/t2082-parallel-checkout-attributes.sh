#!/bin/sh

test_description='parallel-checkout: attributes

Verify that parallel-checkout correctly creates files that require
conversions, as specified in .gitattributes. The main point here is
to check that the conv_attr data is correctly sent to the workers
and that it contains sufficient information to smudge files
properly (without access to the index or attribute stack).
'

TEST_NO_CREATE_REPO=1
. ./test-lib.sh
. "$TEST_DIRECTORY/lib-parallel-checkout.sh"
. "$TEST_DIRECTORY/lib-encoding.sh"

test_expect_success 'parallel-checkout with ident' '
	git init ident &&
	(
		cd ident &&
		echo "A ident" >.gitattributes &&
		echo "\$Id\$" >A &&
		echo "\$Id\$" >B &&
		git add -A &&
		git commit -m id &&

		rm A B &&
		git_pc 2 0 2 reset --hard &&
		hexsz=$(test_oid hexsz) &&
		grep -E "\\\$Id: [0-9a-f]{$hexsz} \\\$" A &&
		grep "\\\$Id\\\$" B
	)
'

test_expect_success 'parallel-checkout with re-encoding' '
	git init encoding &&
	(
		cd encoding &&
		echo text >utf8-text &&
		cat utf8-text | write_utf16 >utf16-text &&

		echo "A working-tree-encoding=UTF-16" >.gitattributes &&
		cp utf16-text A &&
		cp utf16-text B &&
		git add A B .gitattributes &&
		git commit -m encoding &&

		# Check that A (and only A) is stored in UTF-8
		git cat-file -p :A >A.internal &&
		test_cmp_bin utf8-text A.internal &&
		git cat-file -p :B >B.internal &&
		test_cmp_bin utf16-text B.internal &&

		# Check that A is re-encoded during checkout
		rm A B &&
		git_pc 2 0 2 checkout A B &&
		test_cmp_bin utf16-text A
	)
'

test_expect_success 'parallel-checkout with eol conversions' '
	git init eol &&
	(
		cd eol &&
		git config core.autocrlf false &&
		printf "multi\r\nline\r\ntext" >crlf-text &&
		printf "multi\nline\ntext" >lf-text &&

		echo "A text eol=crlf" >.gitattributes &&
		echo "B -text" >>.gitattributes &&
		cp crlf-text A &&
		cp crlf-text B &&
		git add A B .gitattributes &&
		git commit -m eol &&

		# Check that A (and only A) is stored with LF format
		git cat-file -p :A >A.internal &&
		test_cmp_bin lf-text A.internal &&
		git cat-file -p :B >B.internal &&
		test_cmp_bin crlf-text B.internal &&

		# Check that A is converted to CRLF during checkout
		rm A B &&
		git_pc 2 0 2 checkout A B &&
		test_cmp_bin crlf-text A
	)
'

test_cmp_str()
{
	echo "$1" >tmp &&
	test_cmp tmp "$2"
}

# Entries that require an external filter are not eligible for parallel
# checkout. Check that both the parallel-eligible and non-eligible entries are
# properly writen in a single checkout process.
#
test_expect_success 'parallel-checkout and external filter' '
	git init filter &&
	(
		cd filter &&
		git config filter.x2y.clean "tr x y" &&
		git config filter.x2y.smudge "tr y x" &&
		git config filter.x2y.required true &&

		echo "A filter=x2y" >.gitattributes &&
		echo x >A &&
		echo x >B &&
		echo x >C &&
		git add -A &&
		git commit -m filter &&

		# Check that A (and only A) was cleaned
		git cat-file -p :A >A.internal &&
		test_cmp_str y A.internal &&
		git cat-file -p :B >B.internal &&
		test_cmp_str x B.internal &&
		git cat-file -p :C >C.internal &&
		test_cmp_str x C.internal &&

		rm A B C *.internal &&
		git_pc 2 0 2 checkout A B C &&
		test_cmp_str x A &&
		test_cmp_str x B &&
		test_cmp_str x C
	)
'

# The delayed queue is independent from the parallel queue, and they should be
# able to work together in the same checkout process.
#
test_expect_success PERL 'parallel-checkout and delayed checkout' '
	write_script rot13-filter.pl "$PERL_PATH" \
		<"$TEST_DIRECTORY"/t0021/rot13-filter.pl &&
	test_config_global filter.delay.process \
		"\"$(pwd)/rot13-filter.pl\" \"$(pwd)/delayed.log\" clean smudge delay" &&
	test_config_global filter.delay.required true &&

	echo "a b c" >delay-content &&
	echo "n o p" >delay-rot13-content &&

	git init delayed &&
	(
		cd delayed &&
		echo "*.a filter=delay" >.gitattributes &&
		cp ../delay-content test-delay10.a &&
		cp ../delay-content test-delay11.a &&
		echo parallel >parallel1.b &&
		echo parallel >parallel2.b &&
		git add -A &&
		git commit -m delayed &&

		# Check that the stored data was cleaned
		git cat-file -p :test-delay10.a > delay10.internal &&
		test_cmp delay10.internal ../delay-rot13-content &&
		git cat-file -p :test-delay11.a > delay11.internal &&
		test_cmp delay11.internal ../delay-rot13-content &&
		rm *.internal &&

		rm *.a *.b
	) &&

	git_pc 2 0 2 -C delayed checkout -f &&
	verify_checkout delayed &&

	# Check that the *.a files got to the delay queue and were filtered
	grep "smudge test-delay10.a .* \[DELAYED\]" delayed.log &&
	grep "smudge test-delay11.a .* \[DELAYED\]" delayed.log &&
	test_cmp delayed/test-delay10.a delay-content &&
	test_cmp delayed/test-delay11.a delay-content
'

test_done
