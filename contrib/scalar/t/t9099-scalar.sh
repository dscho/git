#!/bin/sh

test_description='test the `scalar` command'

TEST_DIRECTORY=$(pwd)/../../../t
export TEST_DIRECTORY

# Make it work with --no-bin-wrappers
PATH=$(pwd)/..:$PATH

. ../../../t/test-lib.sh

test_expect_success 'scalar shows a usage' '
	test_expect_code 129 scalar -h
'

test_expect_success 'scalar unregister' '
	git init vanish/src &&
	scalar register vanish/src &&
	git config --get --global --fixed-value \
		maintenance.repo "$(pwd)/vanish/src" &&
	scalar list >scalar.repos &&
	grep -F "$(pwd)/vanish/src" scalar.repos &&
	rm -rf vanish/src/.git &&
	scalar unregister vanish &&
	test_must_fail git config --get --global --fixed-value \
		maintenance.repo "$(pwd)/vanish/src" &&
	scalar list >scalar.repos &&
	! grep -F "$(pwd)/vanish/src" scalar.repos
'

test_expect_success '`scalar register` & `unregister` with existing repo' '
	git init existing &&
	scalar register existing &&
	git config --get --global --fixed-value \
		maintenance.repo "$(pwd)/existing" &&
	scalar list >scalar.repos &&
	grep -F "$(pwd)/existing" scalar.repos &&
	scalar unregister existing &&
	test_must_fail git config --get --global --fixed-value \
		maintenance.repo "$(pwd)/existing" &&
	scalar list >scalar.repos &&
	! grep -F "$(pwd)/existing" scalar.repos
'

test_expect_success '`scalar unregister` with existing repo, deleted .git' '
	scalar register existing &&
	rm -rf existing/.git &&
	scalar unregister existing &&
	test_must_fail git config --get --global --fixed-value \
		maintenance.repo "$(pwd)/existing" &&
	scalar list >scalar.repos &&
	! grep -F "$(pwd)/existing" scalar.repos
'

test_expect_success '`scalar register` existing repo with `src` folder' '
	git init existing &&
	mkdir -p existing/src &&
	scalar register existing/src &&
	scalar list >scalar.repos &&
	grep -F "$(pwd)/existing" scalar.repos &&
	scalar unregister existing &&
	scalar list >scalar.repos &&
	! grep -F "$(pwd)/existing" scalar.repos
'

test_done
