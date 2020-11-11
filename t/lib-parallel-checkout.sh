# Helpers for t208* tests

if ! test -z "$GIT_TEST_CHECKOUT_WORKERS"
then
	skip_all="skipping test, GIT_TEST_CHECKOUT_WORKERS is set"
	test_done
fi

# Runs `git -c checkout.workers=$1 -c checkout.thesholdForParallelism=$2 ${@:4}`
# and checks that the number of workers spawned is equal to $3.
#
git_pc()
{
	if test $# -lt 4
	then
		BUG "too few arguments to git_pc()"
	fi &&

	workers=$1 threshold=$2 expected_workers=$3 &&
	shift 3 &&

	rm -f trace &&
	GIT_TRACE2="$(pwd)/trace" git \
		-c checkout.workers=$workers \
		-c checkout.thresholdForParallelism=$threshold \
		-c advice.detachedHead=0 \
		"$@" 2>&8 &&

	# Check that the expected number of workers has been used. Note that it
	# can be different from the requested number in two cases: when the
	# threshold is not reached; and when there are not enough
	# parallel-eligible entries for all workers.
	#
	local workers_in_trace=$(grep "child_start\[..*\] git checkout--helper" trace | wc -l) &&
	test $workers_in_trace -eq $expected_workers &&
	rm -f trace
} 8>&2 2>&4

# Verify that both the working tree and the index were created correctly
verify_checkout()
{
	git -C "$1" diff-index --quiet HEAD -- &&
	git -C "$1" diff-index --quiet --cached HEAD -- &&
	git -C "$1" status --porcelain >"$1".status &&
	test_must_be_empty "$1".status
}
