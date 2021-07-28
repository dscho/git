#!/bin/bash

test_description='config-managed multihooks, including git-hook command'

. ./test-lib.sh

ROOT=
if test_have_prereq MINGW
then
	# In Git for Windows, Unix-like paths work only in shell scripts;
	# `git.exe`, however, will prefix them with the pseudo root directory
	# (of the Unix shell). Let's accommodate for that.
	ROOT="$(cd / && pwd)"
fi

setup_hooks () {
	test_config hook.pre-commit.command "/path/ghi" --add
	test_config_global hook.pre-commit.command "/path/def" --add
}

setup_hookcmd () {
	test_config hook.pre-commit.command "abc" --add
	test_config_global hookcmd.abc.command "/path/abc" --add
}

setup_hookdir () {
	mkdir .git/hooks
	write_script .git/hooks/pre-commit <<-EOF
	echo \"Legacy Hook\"
	EOF
	test_when_finished rm -rf .git/hooks
}

test_expect_success 'git hook rejects commands without a mode' '
	test_must_fail git hook pre-commit
'

test_expect_success 'git hook rejects commands without a hookname' '
	test_must_fail git hook list
'

test_expect_success 'git hook runs outside of a repo' '
	setup_hooks &&

	cat >expected <<-EOF &&
	$ROOT/path/def
	EOF

	nongit git config --list --global &&

	nongit git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list orders by config order' '
	setup_hooks &&

	cat >expected <<-EOF &&
	$ROOT/path/def
	$ROOT/path/ghi
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list dereferences a hookcmd' '
	setup_hooks &&
	setup_hookcmd &&

	cat >expected <<-EOF &&
	$ROOT/path/def
	$ROOT/path/ghi
	$ROOT/path/abc
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list reorders on duplicate commands' '
	setup_hooks &&

	test_config hook.pre-commit.command "/path/def" --add &&

	cat >expected <<-EOF &&
	$ROOT/path/ghi
	$ROOT/path/def
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list shows hooks from the hookdir' '
	setup_hookdir &&

	cat >expected <<-EOF &&
	.git/hooks/pre-commit
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list removes skipped hookcmd' '
	setup_hookcmd &&
	test_config hookcmd.abc.skip "true" --add &&

	cat >expected <<-EOF &&
	no commands configured for hook '\''pre-commit'\''
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list ignores skip referring to unused hookcmd' '
	test_config hookcmd.abc.command "/path/abc" --add &&
	test_config hookcmd.abc.skip "true" --add &&

	cat >expected <<-EOF &&
	no commands configured for hook '\''pre-commit'\''
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'

test_expect_success 'git hook list removes skipped inlined hook' '
	setup_hooks &&
	test_config hookcmd."$ROOT/path/ghi".skip "true" --add &&

	cat >expected <<-EOF &&
	$ROOT/path/def
	EOF

	git hook list pre-commit >actual &&
	test_cmp expected actual
'


test_expect_success 'inline hook definitions execute oneliners' '
	test_config hook.pre-commit.command "echo \"Hello World\"" &&

	echo "Hello World" >expected &&

	# hooks are run with stdout_to_stderr = 1
	git hook run pre-commit 2>actual &&
	test_cmp expected actual
'

test_expect_success 'inline hook definitions resolve paths' '
	write_script sample-hook.sh <<-EOF &&
	echo \"Sample Hook\"
	EOF

	test_when_finished "rm sample-hook.sh" &&

	test_config hook.pre-commit.command "\"$(pwd)/sample-hook.sh\"" &&

	echo \"Sample Hook\" >expected &&

	# hooks are run with stdout_to_stderr = 1
	git hook run pre-commit 2>actual &&
	test_cmp expected actual
'

test_expect_success 'git hook run can pass args' '
	write_script sample-hook.sh <<-\EOF &&
	echo $1
	echo $2
	EOF

	test_config hook.pre-commit.command "\"$(pwd)/sample-hook.sh\"" &&

	cat >expected <<-EOF &&
	arg1
	arg2
	EOF

	git hook run pre-commit -- arg1 arg2 2>actual &&

	test_cmp expected actual
'

test_expect_success 'hookdir hook included in git hook run' '
	setup_hookdir &&

	echo \"Legacy Hook\" >expected &&

	# hooks are run with stdout_to_stderr = 1
	git hook run pre-commit 2>actual &&
	test_cmp expected actual
'

test_expect_success 'out-of-repo runs excluded' '
	setup_hooks &&

	nongit test_must_fail git hook run pre-commit
'

test_expect_success 'stdin to multiple hooks' '
	git config --add hook.test.command "xargs -P1 -I% echo a%" &&
	git config --add hook.test.command "xargs -P1 -I% echo b%" &&
	test_when_finished "test_unconfig hook.test.command" &&

	cat >input <<-EOF &&
	1
	2
	3
	EOF

	cat >expected <<-EOF &&
	a1
	a2
	a3
	b1
	b2
	b3
	EOF

	git hook run --to-stdin=input test 2>actual &&
	test_cmp expected actual
'

test_done
