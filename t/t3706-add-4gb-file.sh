#!/bin/sh
#
# Copyright (c) 2026 Johannes Schindelin
#

test_description='add and archive 4GB+ objects'

. ./test-lib.sh

if ! test_have_prereq EXPENSIVE,SIZE_T_IS_64BIT
then
	skip_all='expensive 4GB blob test; enable on 64-bit with GIT_TEST_LONG=true'
	test_done
fi

size_4gb=4294967296

test_lazy_prereq UNZIP_ZIP64_SUPPORT '
	"$GIT_UNZIP" -v | grep ZIP64_SUPPORT
'

test_lazy_prereq GZIP 'gzip --version'

test_expect_success 'set up a 4GB file' '
	test_atexit "rm -f large" &&
	# genrandom takes only an unsigned long...
	test-tool genrandom 123 $(($size_4gb-1)) >large &&
	printf 1 >>large
'

test_expect_success 'add and commit 4GB file' '
	git add large &&
	git cat-file -s :large >size-staged &&
	test $size_4gb = $(cat size-staged) &&
	git commit -m large-file
'
test_expect_success 'read 4GB loose object' '
	git -P show :large >read &&
	test_file_size read >size-read &&
	test $size_4gb = $(cat size-read)
'

# export-subst disables streaming; archiving a tree avoids substitutions.
test_expect_success UNZIP,UNZIP_ZIP64_SUPPORT \
	'zip archive of 4GB file (buffered)' '
	mkdir -p .git/info &&
	echo "large export-subst" >.git/info/attributes &&
	git archive --format=zip -0 HEAD: >large.zip &&
	"$GIT_UNZIP" -p large.zip large >read &&
	test_file_size read >size-read &&
	test $size_4gb = $(cat size-read)
'

test_expect_success GZIP 'tar.gz archive of 4GB file' '
	git archive --format=tar.gz -0 HEAD >large.tar.gz &&
	gzip -d large.tar.gz &&
	mkdir extract &&
	(cd extract && "$TAR" xf ../large.tar) &&
	test_file_size extract/large >size-large &&
	test $size_4gb = $(cat size-large)
'

test_done
