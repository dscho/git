#!/bin/sh

test_description='ls-tree --format'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

test_expect_success 'ls-tree --format usage' '
	test_expect_code 129 git ls-tree --format=fmt -l HEAD &&
	test_expect_code 129 git ls-tree --format=fmt --name-only HEAD &&
	test_expect_code 129 git ls-tree --format=fmt --name-status HEAD
'

test_expect_success 'setup' '
	mkdir dir &&
	test_commit dir/sub-file &&
	test_commit top-file
'

test_ls_tree_format () {
	format=$1 &&
	opts=$2 &&
	fmtopts=$3 &&
	shift 2 &&
	git ls-tree $opts -r HEAD >expect.raw &&
	sed "s/^/> /" >expect <expect.raw &&
	git ls-tree --format="> $format" -r $fmtopts HEAD >actual &&
	test_cmp expect actual
}

test_expect_success 'ls-tree --format=<default-like>' '
	test_ls_tree_format \
		"%(objectmode) %(objecttype) %(objectname)%x09%(path)" \
		""
'

test_expect_success 'ls-tree --format=<long-like>' '
	test_ls_tree_format \
		"%(objectmode) %(objecttype) %(objectname) %(objectsize:padded)%x09%(path)" \
		"--long"
'

test_expect_success 'ls-tree --format=<name-only-like>' '
	test_ls_tree_format \
		"%(path)" \
		"--name-only"
'

test_expect_success 'ls-tree --format=<object-only-like>' '
	test_ls_tree_format \
		"%(objectname)" \
		"--object-only"
'

test_expect_success 'ls-tree combine --format=<default-like> and -t' '
	test_ls_tree_format \
	"%(objectmode) %(objecttype) %(objectname)%x09%(path)" \
	"-t" \
	"-t"
'

test_expect_success 'ls-tree combine --format=<default-like> and --full-name' '
	test_ls_tree_format \
	"%(objectmode) %(objecttype) %(objectname)%x09%(path)" \
	"--full-name" \
	"--full-name"
'

test_expect_success 'ls-tree combine --format=<default-like> and --full-tree' '
	test_ls_tree_format \
	"%(objectmode) %(objecttype) %(objectname)%x09%(path)" \
	"--full-tree" \
	"--full-tree"
'

test_expect_success 'ls-tree hit fast-path with --format=<default-like>' '
	git ls-tree -r HEAD >expect &&
	git ls-tree --format="%(objectmode) %(objecttype) %(objectname)%x09%(path)" -r HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'ls-tree hit fast-path with --format=<name-only-like>' '
	git ls-tree -r --name-only HEAD >expect &&
	git ls-tree --format="%(path)" -r HEAD >actual &&
	test_cmp expect actual
'

test_expect_success 'ls-tree hit fast-path with --format=<object-only-like>' '
	git ls-tree -r --object-only HEAD >expect &&
	git ls-tree --format="%(objectname)" -r HEAD >actual &&
	test_cmp expect actual
'
test_done
