#!/bin/sh

test_description='git serialized status tests'

. ./test-lib.sh

# This file includes tests for serializing / deserializing
# status data. These tests cover two basic features:
#
# [1] Because users can request different types of untracked-file
#     and ignored file reporting, the cache data generated by
#     serialize must use either the same untracked and ignored
#     parameters as the later deserialize invocation; otherwise,
#     the deserialize invocation must disregard the cached data
#     and run a full scan itself.
#
#     To increase the number of cases where the cached status can
#     be used, we have added a "--untracked-file=complete" option
#     that reports a superset or union of the results from the
#     "-u normal" and "-u all".  We combine this with a filter in
#     deserialize to filter the results.
#
#     Ignored file reporting is simpler in that is an all or
#     nothing; there are no subsets.
#
#     The tests here (in addition to confirming that a cache
#     file can be generated and used by a subsequent status
#     command) need to test this untracked-file filtering.
#
# [2] ensuring the status calls are using data from the status
#     cache as expected.  This includes verifying cached data
#     is used when appropriate as well as falling back to
#     performing a new status scan when the data in the cache
#     is insufficient/known stale.

test_expect_success 'setup' '
	git branch -M main &&
	cat >.gitignore <<-\EOF &&
	*.ign
	ignored_dir/
	EOF

	mkdir tracked ignored_dir &&
	touch tracked_1.txt tracked/tracked_1.txt &&
	git add . &&
	test_tick &&
	git commit -m"Adding original file." &&
	mkdir untracked &&
	touch ignored.ign ignored_dir/ignored_2.txt \
	      untracked_1.txt untracked/untracked_2.txt untracked/untracked_3.txt &&

	test_oid_cache <<-EOF
	branch_oid sha1:68d4a437ea4c2de65800f48c053d4d543b55c410
	x_base sha1:587be6b4c3f93f93c489c0111bba5596147a26cb
	x_ours sha1:b68025345d5301abad4d9ec9166f455243a0d746
	x_theirs sha1:975fbec8256d3e8a3797e7a3611380f27c49f4ac

	branch_oid sha256:6b95e4b1ea911dad213f2020840f5e92d3066cf9e38cf35f79412ec58d409ce4
	x_base sha256:14f5162e2fe3d240d0d37aaab0f90e4af9a7cfa79639f3bab005b5bfb4174d9f
	x_ours sha256:3a404ba030a4afa912155c476a48a253d4b3a43d0098431b6d6ca6e554bd78fb
	x_theirs sha256:44dc634218adec09e34f37839b3840bad8c6103693e9216626b32d00e093fa35
	EOF
'

test_expect_success 'verify untracked-files=complete with no conversion' '
	test_when_finished "rm serialized_status.dat new_change.txt output" &&
	cat >expect <<-\EOF &&
	? expect
	? serialized_status.dat
	? untracked/
	? untracked/untracked_2.txt
	? untracked/untracked_3.txt
	? untracked_1.txt
	! ignored.ign
	! ignored_dir/
	EOF

	git status --untracked-files=complete --ignored=matching --serialize >serialized_status.dat &&
	touch new_change.txt &&

	git status --porcelain=v2 --untracked-files=complete --ignored=matching --deserialize=serialized_status.dat >output &&
	test_cmp expect output
'

test_expect_success 'verify untracked-files=complete to untracked-files=normal conversion' '
	test_when_finished "rm serialized_status.dat new_change.txt output" &&
	cat >expect <<-\EOF &&
	? expect
	? serialized_status.dat
	? untracked/
	? untracked_1.txt
	EOF

	git status --untracked-files=complete --ignored=matching --serialize >serialized_status.dat &&
	touch new_change.txt &&

	git status --porcelain=v2 --deserialize=serialized_status.dat >output &&
	test_cmp expect output
'

test_expect_success 'verify untracked-files=complete to untracked-files=all conversion' '
	test_when_finished "rm serialized_status.dat new_change.txt output" &&
	cat >expect <<-\EOF &&
	? expect
	? serialized_status.dat
	? untracked/untracked_2.txt
	? untracked/untracked_3.txt
	? untracked_1.txt
	! ignored.ign
	! ignored_dir/
	EOF

	git status --untracked-files=complete --ignored=matching --serialize >serialized_status.dat &&
	touch new_change.txt &&

	git status --porcelain=v2 --untracked-files=all --ignored=matching --deserialize=serialized_status.dat >output &&
	test_cmp expect output
'

test_expect_success 'verify serialized status with non-convertible ignore mode does new scan' '
	test_when_finished "rm serialized_status.dat new_change.txt output" &&
	cat >expect <<-\EOF &&
	? expect
	? new_change.txt
	? output
	? serialized_status.dat
	? untracked/
	? untracked_1.txt
	! ignored.ign
	! ignored_dir/
	EOF

	git status --untracked-files=complete --ignored=matching --serialize >serialized_status.dat &&
	touch new_change.txt &&

	git status --porcelain=v2 --ignored --deserialize=serialized_status.dat >output &&
	test_cmp expect output
'

test_expect_success 'verify serialized status handles path scopes' '
	test_when_finished "rm serialized_status.dat new_change.txt output" &&
	cat >expect <<-\EOF &&
	? untracked/
	EOF

	git status --untracked-files=complete --ignored=matching --serialize >serialized_status.dat &&
	touch new_change.txt &&

	git status --porcelain=v2 --deserialize=serialized_status.dat untracked >output &&
	test_cmp expect output
'

test_expect_success 'verify no-ahead-behind and serialized status integration' '
	test_when_finished "rm serialized_status.dat new_change.txt output" &&
	cat >expect <<-EOF &&
	# branch.oid $(test_oid branch_oid)
	# branch.head alt_branch
	# branch.upstream main
	# branch.ab +1 -0
	? expect
	? serialized_status.dat
	? untracked/
	? untracked_1.txt
	EOF

	git checkout -b alt_branch main --track >/dev/null &&
	touch alt_branch_changes.txt &&
	git add alt_branch_changes.txt &&
	test_tick &&
	git commit -m"New commit on alt branch"  &&

	git status --untracked-files=complete --ignored=matching --serialize >serialized_status.dat &&
	touch new_change.txt &&

	git -c status.aheadBehind=false status --porcelain=v2 --branch --ahead-behind --deserialize=serialized_status.dat >output &&
	test_cmp expect output
'

test_expect_success 'verify new --serialize=path mode' '
	test_when_finished "rm serialized_status.dat expect new_change.txt output.1 output.2" &&
	cat >expect <<-\EOF &&
	? expect
	? output.1
	? untracked/
	? untracked_1.txt
	EOF

	git checkout -b serialize_path_branch main --track >/dev/null &&
	touch alt_branch_changes.txt &&
	git add alt_branch_changes.txt &&
	test_tick &&
	git commit -m"New commit on serialize_path_branch"  &&

	git status --porcelain=v2 --serialize=serialized_status.dat >output.1 &&
	touch new_change.txt &&

	git status --porcelain=v2 --deserialize=serialized_status.dat >output.2 &&
	test_cmp expect output.1 &&
	test_cmp expect output.2
'

test_expect_success 'try deserialize-wait feature' '
	test_when_finished "rm -f serialized_status.dat dirt expect.* output.* trace.*" &&

	git status --serialize=serialized_status.dat >output.1 &&

	# make status cache stale by updating the mtime on the index.  confirm that
	# deserialize fails when requested.
	sleep 1 &&
	touch .git/index &&
	test_must_fail git status --deserialize=serialized_status.dat --deserialize-wait=fail &&
	test_must_fail git -c status.deserializeWait=fail status --deserialize=serialized_status.dat &&

	cat >expect.1 <<-\EOF &&
	? expect.1
	? output.1
	? serialized_status.dat
	? untracked/
	? untracked_1.txt
	EOF

	# refresh the status cache.
	git status --porcelain=v2 --serialize=serialized_status.dat >output.1 &&
	test_cmp expect.1 output.1 &&

	# create some dirt. confirm deserialize used the existing status cache.
	echo x >dirt &&
	git status --porcelain=v2 --deserialize=serialized_status.dat >output.2 &&
	test_cmp output.1 output.2 &&

	# make the cache stale and try the timeout feature and wait upto
	# 2 tenths of a second.  confirm deserialize timed out and rejected
	# the status cache and did a normal scan.

	cat >expect.2 <<-\EOF &&
	? dirt
	? expect.1
	? expect.2
	? output.1
	? output.2
	? serialized_status.dat
	? trace.2
	? untracked/
	? untracked_1.txt
	EOF

	sleep 1 &&
	touch .git/index &&
	GIT_TRACE_DESERIALIZE=1 git status --porcelain=v2 --deserialize=serialized_status.dat --deserialize-wait=2 >output.2 2>trace.2 &&
	test_cmp expect.2 output.2 &&
	grep "wait polled=2 result=1" trace.2 >trace.2g
'

test_expect_success 'merge conflicts' '

	# create a merge conflict.

	git init -b main conflicts &&
	echo x >conflicts/x.txt &&
	git -C conflicts add x.txt &&
	git -C conflicts commit -m x &&
	git -C conflicts branch a &&
	git -C conflicts branch b &&
	git -C conflicts checkout a &&
	echo y >conflicts/x.txt &&
	git -C conflicts add x.txt &&
	git -C conflicts commit -m a &&
	git -C conflicts checkout b &&
	echo z >conflicts/x.txt &&
	git -C conflicts add x.txt &&
	git -C conflicts commit -m b &&
	test_must_fail git -C conflicts merge --no-commit a &&

	# verify that regular status correctly identifies it
	# in each format.

	cat >expect.v2 <<EOF &&
u UU N... 100644 100644 100644 100644 $(test_oid x_base) $(test_oid x_ours) $(test_oid x_theirs) x.txt
EOF
	git -C conflicts status --porcelain=v2 >observed.v2 &&
	test_cmp expect.v2 observed.v2 &&

	cat >expect.long <<EOF &&
On branch b
You have unmerged paths.
  (fix conflicts and run "git commit")
  (use "git merge --abort" to abort the merge)

Unmerged paths:
  (use "git add <file>..." to mark resolution)
	both modified:   x.txt

no changes added to commit (use "git add" and/or "git commit -a")
EOF
	git -C conflicts status --long >observed.long &&
	test_cmp expect.long observed.long &&

	cat >expect.short <<EOF &&
UU x.txt
EOF
	git -C conflicts status --short >observed.short &&
	test_cmp expect.short observed.short &&

	# save status data in serialized cache.

	git -C conflicts status --serialize >serialized &&

	# make some dirt in the worktree so we can tell whether subsequent
	# status commands used the cached data or did a fresh status.

	echo dirt >conflicts/dirt.txt &&

	# run status using the cached data.

	git -C conflicts status --long --deserialize=../serialized >observed.long &&
	test_cmp expect.long observed.long &&

	git -C conflicts status --short --deserialize=../serialized >observed.short &&
	test_cmp expect.short observed.short &&

	# currently, the cached data does not have enough information about
	# merge conflicts for porcelain V2 format.  (And V2 format looks at
	# the index to get that data, but the whole point of the serialization
	# is to avoid reading the index unnecessarily.)  So V2 always rejects
	# the cached data when there is an unresolved conflict.

	cat >expect.v2.dirty <<EOF &&
u UU N... 100644 100644 100644 100644 $(test_oid x_base) $(test_oid x_ours) $(test_oid x_theirs) x.txt
? dirt.txt
EOF
	git -C conflicts status --porcelain=v2 --deserialize=../serialized >observed.v2 &&
	test_cmp expect.v2.dirty observed.v2

'

test_expect_success 'renames' '
	git init -b main rename_test &&
	echo OLDNAME >rename_test/OLDNAME &&
	git -C rename_test add OLDNAME &&
	git -C rename_test commit -m OLDNAME &&
	git -C rename_test mv OLDNAME NEWNAME &&
	git -C rename_test status --serialize=renamed.dat >output.1 &&
	echo DIRT >rename_test/DIRT &&
	git -C rename_test status --deserialize=renamed.dat >output.2 &&
	test_cmp output.1 output.2
'

test_done
