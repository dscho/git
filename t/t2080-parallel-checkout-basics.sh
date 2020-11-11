#!/bin/sh

test_description='parallel-checkout basics

Ensure that parallel-checkout basically works on clone and checkout, spawning
the required number of workers and correctly populating both the index and
working tree.
'

TEST_NO_CREATE_REPO=1
. ./test-lib.sh
. "$TEST_DIRECTORY/lib-parallel-checkout.sh"

# Test parallel-checkout with different operations (creation, deletion,
# modification) and entry types. A branch switch from B1 to B2 will contain:
#
# - a (file):      modified
# - e/x (file):    deleted
# - b (symlink):   deleted
# - b/f (file):    created
# - e (symlink):   created
# - d (submodule): created
#
test_expect_success SYMLINKS 'setup repo for checkout with various operations' '
	git init various &&
	(
		cd various &&
		git checkout -b B1 &&
		echo a>a &&
		mkdir e &&
		echo e/x >e/x &&
		ln -s e b &&
		git add -A &&
		git commit -m B1 &&

		git checkout -b B2 &&
		echo modified >a &&
		rm -rf e &&
		rm b &&
		mkdir b &&
		echo b/f >b/f &&
		ln -s b e &&
		git init d &&
		test_commit -C d f &&
		git submodule add ./d &&
		git add -A &&
		git commit -m B2 &&

		git checkout --recurse-submodules B1
	)
'

test_expect_success SYMLINKS 'sequential checkout' '
	cp -R various various_sequential &&
	git_pc 1 0 0 -C various_sequential checkout --recurse-submodules B2 &&
	verify_checkout various_sequential
'

test_expect_success SYMLINKS 'parallel checkout' '
	cp -R various various_parallel &&
	git_pc 2 0 2 -C various_parallel checkout --recurse-submodules B2 &&
	verify_checkout various_parallel
'

test_expect_success SYMLINKS 'fallback to sequential checkout (threshold)' '
	cp -R various various_sequential_fallback &&
	git_pc 2 100 0 -C various_sequential_fallback checkout --recurse-submodules B2 &&
	verify_checkout various_sequential_fallback
'

test_expect_success SYMLINKS 'parallel checkout on clone' '
	git -C various checkout --recurse-submodules B2 &&
	git_pc 2 0 2 clone --recurse-submodules various various_parallel_clone  &&
	verify_checkout various_parallel_clone
'

test_expect_success SYMLINKS 'fallback to sequential checkout on clone (threshold)' '
	git -C various checkout --recurse-submodules B2 &&
	git_pc 2 100 0 clone --recurse-submodules various various_sequential_fallback_clone &&
	verify_checkout various_sequential_fallback_clone
'

# Just to be paranoid, actually compare the working trees' contents directly.
test_expect_success SYMLINKS 'compare the working trees' '
	rm -rf various_*/.git &&
	rm -rf various_*/d/.git &&

	diff -r various_sequential various_parallel &&
	diff -r various_sequential various_sequential_fallback &&
	diff -r various_sequential various_parallel_clone &&
	diff -r various_sequential various_sequential_fallback_clone
'

test_cmp_str()
{
	echo "$1" >tmp &&
	test_cmp tmp "$2"
}

test_expect_success 'parallel checkout respects --[no]-force' '
	git init dirty &&
	(
		cd dirty &&
		mkdir D &&
		test_commit D/F &&
		test_commit F &&

		echo changed >F.t &&
		rm -rf D &&
		echo changed >D &&

		# We expect 0 workers because there is nothing to be updated
		git_pc 2 0 0 checkout HEAD &&
		test_path_is_file D &&
		test_cmp_str changed D &&
		test_cmp_str changed F.t &&

		git_pc 2 0 2 checkout --force HEAD &&
		test_path_is_dir D &&
		test_cmp_str D/F D/F.t &&
		test_cmp_str F F.t
	)
'

test_expect_success SYMLINKS 'parallel checkout checks for symlinks in leading dirs' '
	git init symlinks &&
	(
		cd symlinks &&
		mkdir D E &&

		# Create two entries in D to have enough work for 2 parallel
		# workers
		test_commit D/A &&
		test_commit D/B &&
		test_commit E/C &&
		rm -rf D &&
		ln -s E D &&

		git_pc 2 0 2 checkout --force HEAD &&
		! test -L D &&
		test_cmp_str D/A D/A.t &&
		test_cmp_str D/B D/B.t
	)
'

test_expect_success SYMLINKS,CASE_INSENSITIVE_FS 'symlink colliding with leading dir' '
	git init colliding-symlink &&
	(
		cd colliding-symlink &&
		file_hex=$(git hash-object -w --stdin </dev/null) &&
		file_oct=$(echo $file_hex | hex2oct) &&

		sym_hex=$(echo "./D" | git hash-object -w --stdin) &&
		sym_oct=$(echo $sym_hex | hex2oct) &&

		printf "100644 D/A\0${file_oct}" >tree &&
		printf "100644 E/B\0${file_oct}" >>tree &&
		printf "120000 e\0${sym_oct}" >>tree &&

		tree_hex=$(git hash-object -w -t tree --stdin <tree) &&
		commit_hex=$(git commit-tree -m collisions $tree_hex) &&
		git update-ref refs/heads/colliding-symlink $commit_hex &&

		git_pc 2 0 2 checkout colliding-symlink &&
		test_path_is_dir D &&
		test_path_is_missing D/B
	)
'

test_done
