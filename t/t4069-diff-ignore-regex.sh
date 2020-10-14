#!/bin/sh

test_description='Test diff -I<regex>'

. ./test-lib.sh
. "$TEST_DIRECTORY"/diff-lib.sh

test_expect_success setup '
	test_seq 20 >x &&
	git update-index --add x
'

test_expect_success 'one line changed' '
	test_seq 20 | sed "s/10/100/" >x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -7,7 +7,7 @@
	 7
	 8
	 9
	-10
	+100
	 11
	 12
	 13
	EOF
	compare_diff_patch expected plain &&

	# Both old and new line match regex - ignore change
	git diff -I "^10" >actual &&
	test_must_be_empty actual &&

	# Both old and new line match some regex - ignore change
	git diff -I "^10\$" -I "^100" >actual &&
	test_must_be_empty actual &&

	# Only old line matches regex - do not ignore change
	git diff -I "^10\$" >actual &&
	compare_diff_patch plain actual &&

	# Only new line matches regex - do not ignore change
	git diff -I "^100" >actual &&
	compare_diff_patch plain actual &&

	# Only old line matches some regex - do not ignore change
	git diff -I "^10\$" -I "^101" >actual &&
	compare_diff_patch plain actual &&

	# Only new line matches some regex - do not ignore change
	git diff -I "^11\$" -I "^100" >actual &&
	compare_diff_patch plain actual
'

test_expect_success 'one line removed' '
	test_seq 20 | sed "10d" >x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -7,7 +7,6 @@
	 7
	 8
	 9
	-10
	 11
	 12
	 13
	EOF
	compare_diff_patch expected plain &&

	# Removed line matches regex - ignore change
	git diff -I "^10" >actual &&
	test_must_be_empty actual &&

	# Removed line matches some regex - ignore change
	git diff -I "^10" -I "^100" >actual &&
	test_must_be_empty actual &&

	# Removed line does not match regex - do not ignore change
	git diff -I "^101" >actual &&
	compare_diff_patch plain actual &&

	# Removed line does not match any regex - do not ignore change
	git diff -I "^100" -I "^101" >actual &&
	compare_diff_patch plain actual
'

test_expect_success 'one line added' '
	test_seq 21 >x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -18,3 +18,4 @@
	 18
	 19
	 20
	+21
	EOF
	compare_diff_patch expected plain &&

	# Added line matches regex - ignore change
	git diff -I "^21" >actual &&
	test_must_be_empty actual &&

	# Added line matches some regex - ignore change
	git diff -I "^21" -I "^22" >actual &&
	test_must_be_empty actual &&

	# Added line does not match regex - do not ignore change
	git diff -I "^212" >actual &&
	compare_diff_patch plain actual &&

	# Added line does not match any regex - do not ignore change
	git diff -I "^211" -I "^212" >actual &&
	compare_diff_patch plain actual
'

test_expect_success 'last two lines changed' '
	test_seq 20 | sed "s/19/21/; s/20/22/" >x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -16,5 +16,5 @@
	 16
	 17
	 18
	-19
	-20
	+21
	+22
	EOF
	compare_diff_patch expected plain &&

	# All changed lines match regex - ignore change
	git diff -I "^[12]" >actual &&
	test_must_be_empty actual &&

	# All changed lines match some regex - ignore change
	git diff -I "^1" -I "^2" >actual &&
	test_must_be_empty actual &&

	# Not all changed lines match regex - do not ignore change
	git diff -I "^2" >actual &&
	compare_diff_patch plain actual &&

	# Not all changed lines match some regex - do not ignore change
	git diff -I "^2" -I "^3" >actual &&
	compare_diff_patch plain actual
'

test_expect_success 'two non-adjacent lines removed in the same hunk' '
	test_seq 20 | sed "1d; 3d" >x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -1,6 +1,4 @@
	-1
	 2
	-3
	 4
	 5
	 6
	EOF
	compare_diff_patch expected plain &&

	# Both removed lines match regex - ignore hunk
	git diff -I "^[1-3]" >actual &&
	test_must_be_empty actual &&

	# Both removed lines match some regex - ignore hunk
	git diff -I "^1" -I "^3" >actual &&
	test_must_be_empty actual &&

	# First removed line does not match regex - do not ignore hunk
	git diff -I "^[2-3]" >actual &&
	compare_diff_patch plain actual &&

	# First removed line does not match any regex - do not ignore hunk
	git diff -I "^2" -I "^3" >actual &&
	compare_diff_patch plain actual &&

	# Second removed line does not match regex - do not ignore hunk
	git diff -I "^[1-2]" >actual &&
	compare_diff_patch plain actual &&

	# Second removed line does not match any regex - do not ignore hunk
	git diff -I "^1" -I "^2" >actual &&
	compare_diff_patch plain actual
'

test_expect_success 'two non-adjacent lines removed in the same hunk, with -U1' '
	test_seq 20 | sed "1d; 3d" >x &&

	# Get plain diff
	git diff -U1 >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -1,4 +1,2 @@
	-1
	 2
	-3
	 4
	EOF
	compare_diff_patch expected plain &&

	# Both removed lines match regex - ignore hunk
	git diff -U1 -I "^[1-3]" >actual &&
	test_must_be_empty actual &&

	# Both removed lines match some regex - ignore hunk
	git diff -U1 -I "^1" -I "^3" >actual &&
	test_must_be_empty actual &&

	# First removed line does not match regex, but is out of context - ignore second change
	git diff -U1 -I "^[2-3]" >actual &&
	cat >second-change-ignored <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -1,2 +1 @@
	-1
	 2
	EOF
	compare_diff_patch second-change-ignored actual &&

	# First removed line does not match any regex, but is out of context - ignore second change
	git diff -U1 -I "^2" -I "^3" >actual &&
	compare_diff_patch second-change-ignored actual &&

	# Second removed line does not match regex, but is out of context - ignore first change
	git diff -U1 -I "^[1-2]" >actual &&
	cat >first-change-ignored <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -2,3 +1,2 @@
	 2
	-3
	 4
	EOF
	compare_diff_patch first-change-ignored actual &&

	# Second removed line does not match any regex, but is out of context - ignore first change
	git diff -U1 -I "^1" -I "^2" >actual &&
	compare_diff_patch first-change-ignored actual
'

test_expect_success 'multiple hunks' '
	test_seq 20 | sed "1d; 20d" >x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -1,4 +1,3 @@
	-1
	 2
	 3
	 4
	@@ -17,4 +16,3 @@
	 17
	 18
	 19
	-20
	EOF
	compare_diff_patch expected plain &&

	# Ignore both hunks (single regex)
	git diff -I "^[12]" >actual &&
	test_must_be_empty actual &&

	# Ignore both hunks (multiple regexes)
	git diff -I "^1" -I "^2" >actual &&
	test_must_be_empty actual &&

	# Only ignore first hunk (single regex)
	git diff -I "^1" >actual &&
	cat >first-hunk-ignored <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -17,4 +16,3 @@
	 17
	 18
	 19
	-20
	EOF
	compare_diff_patch first-hunk-ignored actual &&

	# Only ignore first hunk (multiple regexes)
	git diff -I "^0" -I "^1" >actual &&
	compare_diff_patch first-hunk-ignored actual &&

	# Only ignore second hunk (single regex)
	git diff -I "^2" >actual &&
	cat >second-hunk-ignored <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -1,4 +1,3 @@
	-1
	 2
	 3
	 4
	EOF
	compare_diff_patch second-hunk-ignored actual &&

	# Only ignore second hunk (multiple regexes)
	git diff -I "^2" -I "^3" >actual &&
	compare_diff_patch second-hunk-ignored actual
'

test_expect_success 'multiple hunks, with --ignore-blank-lines' '
	echo >x &&
	test_seq 21 >>x &&

	# Get plain diff
	git diff >plain &&
	cat >expected <<-EOF &&
	diff --git a/x b/x
	--- a/x
	+++ b/x
	@@ -1,3 +1,4 @@
	+
	 1
	 2
	 3
	@@ -18,3 +19,4 @@
	 18
	 19
	 20
	+21
	EOF
	compare_diff_patch expected plain &&

	# -I does not override --ignore-blank-lines - ignore both hunks (single regex)
	git diff --ignore-blank-lines -I "^21" >actual &&
	test_must_be_empty actual &&

	# -I does not override --ignore-blank-lines - ignore both hunks (multiple regexes)
	git diff --ignore-blank-lines -I "^21" -I "^12" >actual &&
	test_must_be_empty actual
'

test_expect_success 'diffstat' '
	test_seq 20 | sed "s/^5/0/p; s/^15/10/; 16d" >x &&

	# Get plain diffstat
	git diff --stat >actual &&
	cat >expected <<-EOF &&
	 x | 6 +++---
	 1 file changed, 3 insertions(+), 3 deletions(-)
	EOF
	test_cmp expected actual &&

	# Ignore both hunks (single regex)
	git diff --stat -I "^[0-5]" >actual &&
	test_must_be_empty actual &&

	# Ignore both hunks (multiple regexes)
	git diff --stat -I "^0" -I "^1" -I "^5" >actual &&
	test_must_be_empty actual &&

	# Only ignore first hunk (single regex)
	git diff --stat -I "^[05]" >actual &&
	cat >expected <<-EOF &&
	 x | 3 +--
	 1 file changed, 1 insertion(+), 2 deletions(-)
	EOF
	test_cmp expected actual &&

	# Only ignore first hunk (multiple regexes)
	git diff --stat -I "^0" -I "^5" >actual &&
	test_cmp expected actual &&

	# Only ignore second hunk (single regex)
	git diff --stat -I "^1" >actual &&
	cat >expected <<-EOF &&
	 x | 3 ++-
	 1 file changed, 2 insertions(+), 1 deletion(-)
	EOF
	test_cmp expected actual &&

	# Only ignore second hunk (multiple regexes)
	git diff --stat -I "^1" -I "^2" >actual &&
	test_cmp expected actual
'

test_expect_success 'invalid regexes' '
	>x &&

	# Single invalid regex
	git diff -I "^[1" 2>&1 | grep "invalid regex: " &&

	# Two regexes: first invalid, second valid
	git diff -I "^[1" -I "^1" 2>&1 | grep "invalid regex: " &&

	# Two invalid regexes
	git diff -I "^[1" -I "^[2" 2>&1 | grep "invalid regex: "
'

test_done
