#!/bin/sh
#
# Test Git in parallel
#

. ${0%/*}/lib.sh

group "Run tests" make --quiet -C t T="$(cd t &&
	./helper/test-tool path-utils slice-tests "$1" "$2" t[0-9]*.sh |
	tr '\n' ' ')" &&

# Run the git subtree tests only if main tests succeeded
if test 0 = "$1"
then
	make -C contrib/subtree test
fi ||
handle_failed_tests

check_unignored_build_artifacts
