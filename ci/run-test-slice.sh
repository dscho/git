#!/bin/sh
#
# Test Git in parallel
#

. ${0%/*}/lib.sh

extra=
case "$(uname -s)" in
MINGW*BusyBox*)
	extra='SHELL=/mingw64/bin/ash.exe TEST_SHELL_PATH=/mingw64/bin/ash.exe'
	;;
esac

group "Run tests" make --quiet -C t $extra T="$(cd t &&
	./helper/test-tool path-utils slice-tests "$1" "$2" t[0-9]*.sh |
	tr '\n' ' ')" &&

# Run the git subtree tests only if main tests succeeded
if test 0 = "$1"
then
	make -C contrib/subtree $extra test
fi ||
handle_failed_tests

check_unignored_build_artifacts
