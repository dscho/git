#!/bin/sh

test_description='parallel-checkout collisions

When there are path collisions during a clone, Git should report a warning
listing all of the colliding entries. The sequential code detects a collision
by calling lstat() before trying to open(O_CREAT) the file. Then, to find the
colliding pair of an item k, it searches cache_entry[0, k-1].

This is not sufficient in parallel checkout since:

- A colliding file may be created between the lstat() and open() calls;
- A colliding entry might appear in the second half of the cache_entry array.

The tests in this file make sure that the collision detection code is extended
for parallel checkout.
'

. ./test-lib.sh
. "$TEST_DIRECTORY/lib-parallel-checkout.sh"

TEST_ROOT="$PWD"

test_expect_success CASE_INSENSITIVE_FS 'setup' '
	file_x_hex=$(git hash-object -w --stdin </dev/null) &&
	file_x_oct=$(echo $file_x_hex | hex2oct) &&

	attr_hex=$(echo "file_x filter=logger" | git hash-object -w --stdin) &&
	attr_oct=$(echo $attr_hex | hex2oct) &&

	printf "100644 FILE_X\0${file_x_oct}" >tree &&
	printf "100644 FILE_x\0${file_x_oct}" >>tree &&
	printf "100644 file_X\0${file_x_oct}" >>tree &&
	printf "100644 file_x\0${file_x_oct}" >>tree &&
	printf "100644 .gitattributes\0${attr_oct}" >>tree &&

	tree_hex=$(git hash-object -w -t tree --stdin <tree) &&
	commit_hex=$(git commit-tree -m collisions $tree_hex) &&
	git update-ref refs/heads/collisions $commit_hex &&

	write_script "$TEST_ROOT"/logger_script <<-\EOF
	echo "$@" >>filter.log
	EOF
'

for mode in parallel sequential-fallback
do

	case $mode in
	parallel)		workers=2 threshold=0 expected_workers=2 ;;
	sequential-fallback)	workers=2 threshold=100 expected_workers=0 ;;
	esac

	test_expect_success CASE_INSENSITIVE_FS "collision detection on $mode clone" '
		git_pc $workers $threshold $expected_workers \
			clone --branch=collisions . $mode 2>$mode.stderr &&

		grep FILE_X $mode.stderr &&
		grep FILE_x $mode.stderr &&
		grep file_X $mode.stderr &&
		grep file_x $mode.stderr &&
		test_i18ngrep "the following paths have collided" $mode.stderr
	'

	# The following test ensures that the collision detection code is
	# correctly looking for colliding peers in the second half of the
	# cache_entry array. This is done by defining a smudge command for the
	# *last* array entry, which makes it non-eligible for parallel-checkout.
	# The last entry is then checked out *before* any worker is spawned,
	# making it succeed and the workers' entries collide.
	#
	# Note: this test don't work on Windows because, on this system,
	# collision detection uses strcmp() when core.ignoreCase=false. And we
	# have to set core.ignoreCase=false so that only 'file_x' matches the
	# pattern of the filter attribute. But it works on OSX, where collision
	# detection uses inode.
	#
	test_expect_success CASE_INSENSITIVE_FS,!MINGW,!CYGWIN "collision detection on $mode clone w/ filter" '
		git_pc $workers $threshold $expected_workers \
			-c core.ignoreCase=false \
			-c filter.logger.smudge="\"$TEST_ROOT/logger_script\" %f" \
			clone --branch=collisions . ${mode}_with_filter \
			2>${mode}_with_filter.stderr &&

		grep FILE_X ${mode}_with_filter.stderr &&
		grep FILE_x ${mode}_with_filter.stderr &&
		grep file_X ${mode}_with_filter.stderr &&
		grep file_x ${mode}_with_filter.stderr &&
		test_i18ngrep "the following paths have collided" ${mode}_with_filter.stderr &&

		# Make sure only "file_x" was filtered
		test_path_is_file ${mode}_with_filter/filter.log &&
		echo file_x >expected.filter.log &&
		test_cmp ${mode}_with_filter/filter.log expected.filter.log
	'
done

test_done
