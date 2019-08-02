#!/bin/sh

test_description='simple command server'

. ./test-lib.sh

test-tool simple-ipc SUPPORTS_SIMPLE_IPC || {
	skip_all='simple IPC not supported on this platform'
	test_done
}

stop_simple_IPC_server () {
	test -n "$SIMPLE_IPC_PID" || return 0

	kill "$SIMPLE_IPC_PID" &&
	SIMPLE_IPC_PID=
}

test_expect_success 'start simple command server' '
	{ test-tool simple-ipc daemon & } &&
	SIMPLE_IPC_PID=$! &&
	test_atexit stop_simple_IPC_server
'

test_expect_success 'simple command server' '
	test-tool simple-ipc send ping >actual &&
	echo pong >expect &&
	test_cmp expect actual
'

test_expect_success '`quit` works' '
	test-tool simple-ipc send quit &&
	test_must_fail test-tool simple-ipc send ping
'

test_done

