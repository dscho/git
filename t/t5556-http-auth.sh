#!/bin/sh

test_description='test http auth header and credential helper interop'

. ./test-lib.sh

test_set_port GIT_TEST_HTTP_PROTOCOL_PORT

# Setup a repository
#
REPO_DIR="$(pwd)"/repo

# Setup some lookback URLs where test-http-server will be listening.
# We will spawn it directly inside the repo directory, so we avoid
# any need to configure directory mappings etc - we only serve this
# repository from the root '/' of the server.
#
HOST_PORT=127.0.0.1:$GIT_TEST_HTTP_PROTOCOL_PORT
ORIGIN_URL=http://$HOST_PORT/

# The pid-file is created by test-http-server when it starts.
# The server will shutdown if/when we delete it (this is easier than
# killing it by PID).
#
PID_FILE="$(pwd)"/pid-file.pid
SERVER_LOG="$(pwd)"/OUT.server.log

PATH="$GIT_BUILD_DIR/t/helper/:$PATH" && export PATH
CREDENTIAL_HELPER="$GIT_BUILD_DIR/t/helper/test-credential-helper-replay.sh" \
	&& export CREDENTIAL_HELPER

test_expect_success 'setup repos' '
	test_create_repo "$REPO_DIR" &&
	git -C "$REPO_DIR" branch -M main
'

stop_http_server () {
	if ! test -f "$PID_FILE"
	then
		return 0
	fi
	#
	# The server will shutdown automatically when we delete the pid-file.
	#
	rm -f "$PID_FILE"
	#
	# Give it a few seconds to shutdown (mainly to completely release the
	# port before the next test start another instance and it attempts to
	# bind to it).
	#
	for k in 0 1 2 3 4
	do
		if grep -q "Starting graceful shutdown" "$SERVER_LOG"
		then
			return 0
		fi
		sleep 1
	done

	echo "stop_http_server: timeout waiting for server shutdown"
	return 1
}

start_http_server () {
	#
	# Launch our server into the background in repo_dir.
	#
	(
		cd "$REPO_DIR"
		test-http-server --verbose \
			--listen=127.0.0.1 \
			--port=$GIT_TEST_HTTP_PROTOCOL_PORT \
			--reuseaddr \
			--pid-file="$PID_FILE" \
			"$@" \
			2>"$SERVER_LOG" &
	)
	#
	# Give it a few seconds to get started.
	#
	for k in 0 1 2 3 4
	do
		if test -f "$PID_FILE"
		then
			return 0
		fi
		sleep 1
	done

	echo "start_http_server: timeout waiting for server startup"
	return 1
}

per_test_cleanup () {
	stop_http_server &&
	rm -f OUT.* &&
	rm -f *.cred
}

test_expect_success 'http auth anonymous no challenge' '
	test_when_finished "per_test_cleanup" &&
	start_http_server --allow-anonymous &&

	# Attempt to read from a protected repository
	git ls-remote $ORIGIN_URL
'

test_expect_success 'http auth www-auth headers to credential helper bearer valid' '
	test_when_finished "per_test_cleanup" &&
	start_http_server \
		--auth=bearer:authority=\"id.example.com\"\ q=1\ p=0 \
		--auth=basic:realm=\"example.com\" \
		--auth-token=bearer:secret-token &&

	cat >get-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	wwwauth[]=bearer authority="id.example.com" q=1 p=0
	wwwauth[]=basic realm="example.com"
	EOF

	cat >store-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=secret-token
	authtype=bearer
	EOF

	cat >get-response.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=secret-token
	authtype=bearer
	EOF

	git -c credential.helper="$CREDENTIAL_HELPER" ls-remote $ORIGIN_URL &&

	test_cmp get-expected.cred get-actual.cred &&
	test_cmp store-expected.cred store-actual.cred
'

test_expect_success 'http auth www-auth headers to credential helper basic valid' '
	test_when_finished "per_test_cleanup" &&
	USERPASS64=$(printf "alice:secret-passwd" | base64) &&
	export USERPASS64 &&

	start_http_server \
		--auth=bearer:authority=\"id.example.com\"\ q=1\ p=0 \
		--auth=basic:realm=\"example.com\" \
		--auth-token=basic:$USERPASS64 &&

	cat >get-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	wwwauth[]=bearer authority="id.example.com" q=1 p=0
	wwwauth[]=basic realm="example.com"
	EOF

	cat >store-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=secret-passwd
	authtype=basic
	EOF

	cat >get-response.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=secret-passwd
	authtype=basic
	EOF

	git -c credential.helper="$CREDENTIAL_HELPER" ls-remote $ORIGIN_URL &&

	test_cmp get-expected.cred get-actual.cred &&
	test_cmp store-expected.cred store-actual.cred
'

test_expect_success 'http auth www-auth headers to credential helper custom scheme' '
	test_when_finished "per_test_cleanup" &&
	start_http_server \
		--auth=foobar:alg=test\ widget=1 \
		--auth=bearer:authority=\"id.example.com\"\ q=1\ p=0 \
		--auth=basic:realm=\"example.com\" \
		--auth-token=foobar:SECRET-FOOBAR-VALUE &&

	cat >get-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	wwwauth[]=foobar alg=test widget=1
	wwwauth[]=bearer authority="id.example.com" q=1 p=0
	wwwauth[]=basic realm="example.com"
	EOF

	cat >store-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=SECRET-FOOBAR-VALUE
	authtype=foobar
	EOF

	cat >get-response.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=SECRET-FOOBAR-VALUE
	authtype=foobar
	EOF

	git -c credential.helper="$CREDENTIAL_HELPER" ls-remote $ORIGIN_URL &&

	test_cmp get-expected.cred get-actual.cred &&
	test_cmp store-expected.cred store-actual.cred
'

test_expect_success 'http auth www-auth headers to credential helper invalid' '
	test_when_finished "per_test_cleanup" &&
	start_http_server \
		--auth=bearer:authority=\"id.example.com\"\ q=1\ p=0 \
		--auth=basic:realm=\"example.com\" \
		--auth-token=bearer:secret-token &&

	cat >get-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	wwwauth[]=bearer authority="id.example.com" q=1 p=0
	wwwauth[]=basic realm="example.com"
	EOF

	cat >erase-expected.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=invalid-token
	authtype=bearer
	wwwauth[]=bearer authority="id.example.com" q=1 p=0
	wwwauth[]=basic realm="example.com"
	EOF

	cat >get-response.cred <<-EOF &&
	protocol=http
	host=$HOST_PORT
	username=alice
	password=invalid-token
	authtype=bearer
	EOF

	test_must_fail git -c credential.helper="$CREDENTIAL_HELPER" ls-remote $ORIGIN_URL &&

	test_cmp get-expected.cred get-actual.cred &&
	test_cmp erase-expected.cred erase-actual.cred
'

test_done
