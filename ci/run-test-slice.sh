#!/bin/sh
#
# Test Git in parallel
#

. ${0%/*}/lib.sh

TESTS=$(cd t && ./helper/test-tool path-utils slice-tests "$1" "$2" t[0-9]*.sh)

# On Windows, `prove` is a Perl/MSYS2 harness whose per-test fork+exec
# overhead is dominated by Windows process-creation cost and skews the
# observed parallelism badly. Invoke test-tool's own testsuite runner
# (a native Windows binary) directly so each shell test costs only the
# shell spawn we actually want to measure. The --prove-style flag turns
# the runner into a `prove --timer` look-alike: one timed line per test,
# per-test stdout/stderr discarded, with test-results/<name>.out from
# test-lib's -V mode preserved for failure diagnosis.
if test "$CI_OS_NAME" = "windows"
then
	# MSYS2 rewrites MINGW_PREFIX for native children. Let the test
	# shells derive the POSIX value from MSYSTEM instead.
	unset MINGW_PREFIX

	# MSYS2 bash spawned via a Win32-API CreateProcess (which test-tool
	# uses) reports `pwd` in Windows-style form ("D:/..."), so test-lib's
	# subsequent `PATH=$GIT_BUILD_DIR/bin-wrappers:...` produces an entry
	# the shell cannot tokenise (the drive-letter colon collides with the
	# `:` separator) and `test-tool` becomes unfindable from within tests.
	# Pre-set TEST_DIRECTORY to a POSIX-form path so test-lib never has to
	# call pwd to discover it.
	export TEST_DIRECTORY="$(cd t && pwd)"
	test_args=
	for opt in $GIT_TEST_OPTS
	do
		test_args="$test_args --test-arg=$opt"
	done
	group "Run tests" sh -c '
		cd t &&
		./helper/test-tool run-command testsuite \
			--jobs "$1" --prove-style $2 \
			$(echo "$3" | tr "\n" " ")
	' run-tests "$JOBS" "$test_args" "$TESTS" ||
	handle_failed_tests
else
	group "Run tests" make --quiet -C t \
		T="$(echo "$TESTS" | tr '\n' ' ')" ||
	handle_failed_tests
fi

# We only have one unit test at the moment, so run it in the first slice
if [ "$1" == "0" ] ; then
	group "Run unit tests" make --quiet -C t unit-tests-test-tool
fi

# Run the git subtree tests only if main tests succeeded
test 0 != "$1" || make -C contrib/subtree test

check_unignored_build_artifacts
