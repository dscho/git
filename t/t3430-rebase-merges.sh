#!/bin/sh
#
# Copyright (c) 2018 Johannes E. Schindelin
#

test_description='git rebase -i --rebase-merges

This test runs git rebase "interactively", retaining the branch structure by
recreating merge commits.

Initial setup:

    -- B --                   (first)
   /       \
 A - C - D - E - H            (master)
       \       /
         F - G                (second)
'
. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-rebase.sh

test_cmp_graph () {
	cat >expect &&
	git log --graph --boundary --format=%s "$@" >output &&
	sed "s/ *$//" <output >output.trimmed &&
	test_cmp expect output.trimmed
}

test_expect_success 'setup' '
	write_script replace-editor.sh <<-\EOF &&
	mv "$1" "$(git rev-parse --git-path ORIGINAL-TODO)"
	cp script-from-scratch "$1"
	EOF

	test_commit A &&
	git checkout -b first &&
	test_commit B &&
	git checkout master &&
	test_commit C &&
	test_commit D &&
	git merge --no-commit B &&
	test_tick &&
	git commit -m E &&
	git tag -m E E &&
	git checkout -b second C &&
	test_commit F &&
	test_commit G &&
	git checkout master &&
	git merge --no-commit G &&
	test_tick &&
	git commit -m H &&
	git tag -m H H
'

cat >script-from-scratch <<\EOF
label onto

# onebranch
pick G
pick D
label onebranch

# second
reset onto
pick B
label second

reset onto
merge -C H second
merge onebranch # Merge the topic branch 'onebranch'
EOF

test_expect_success 'create completely different structure' '
	test_config sequence.editor \""$PWD"/replace-editor.sh\" &&
	test_tick &&
	git rebase -i -r A &&
	test_cmp_graph <<-\EOF
	*   Merge the topic branch '\''onebranch'\''
	|\
	| * D
	| * G
	* |   H
	|\ \
	| |/
	|/|
	| * B
	|/
	* A
	EOF
'

test_expect_success 'generate correct todo list' '
	cat >expect <<-\EOF &&
	label onto

	reset onto
	pick d9df450 B
	label E

	reset onto
	pick 5dee784 C
	label branch-point
	pick ca2c861 F
	pick 088b00a G
	label H

	reset branch-point # C
	pick 12bd07b D
	merge -C 2051b56 E # E
	merge -C 233d48a H # H

	EOF

	grep -v "^#" <.git/ORIGINAL-TODO >output &&
	test_cmp expect output
'

test_expect_success 'with a branch tip that was cherry-picked already' '
	git checkout -b already-upstream master &&
	base="$(git rev-parse --verify HEAD)" &&

	test_commit A1 &&
	test_commit A2 &&
	git reset --hard $base &&
	test_commit B1 &&
	test_tick &&
	git merge -m "Merge branch A" A2 &&

	git checkout -b upstream-with-a2 $base &&
	test_tick &&
	git cherry-pick A2 &&

	git checkout already-upstream &&
	test_tick &&
	git rebase -i -r upstream-with-a2 &&
	test_cmp_graph upstream-with-a2.. <<-\EOF
	*   Merge branch A
	|\
	| * A1
	* | B1
	|/
	o A2
	EOF
'

test_expect_success 'refs/rewritten/* is worktree-local' '
	git worktree add wt &&
	cat >wt/script-from-scratch <<-\EOF &&
	label xyz
	exec GIT_DIR=../.git git rev-parse --verify refs/rewritten/xyz >a || :
	exec git rev-parse --verify refs/rewritten/xyz >b
	EOF

	test_config -C wt sequence.editor \""$PWD"/replace-editor.sh\" &&
	git -C wt rebase -i HEAD &&
	test_must_be_empty wt/a &&
	test_cmp_rev HEAD "$(cat wt/b)"
'

test_done
