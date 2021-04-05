# Shell library sourced instead of ./test-lib.sh by tests that need
# to run under Bash; primarily intended for tests of the completion
# script.

if test -n "$BASH" && test -z "$POSIXLY_CORRECT"
then
	# we are in full-on bash mode
	true
elif type bash >/dev/null 2>&1
then
	# execute in full-on bash mode
	unset POSIXLY_CORRECT
	test -z "$GIT_TEST_USE_BUSYBOX" ||
	GIT_TEST_USE_BUSYBOX= exec /usr/bin/bash "$0" "$@" ||
	exit 1
	exec bash "$0" "$@"
else
	echo '1..0 #SKIP skipping bash completion tests; bash not available'
	exit 0
fi

. ./test-lib.sh
