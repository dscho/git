#!/bin/sh

# Script to simulate a transient error code with Retry-After header set.
#
# PATH_INFO must be of the form /<nonce>/<times>/<retcode>/<retry-after>/<path>
#   (eg: /dc724af1/3/429/10/some/url)
#
# The <nonce> value uniquely identifies the URL, since we're simulating
# a stateful operation using a stateless protocol, we need a way to "namespace"
# URLs so that they don't step on each other.
#
# The first <times> times this endpoint is called, it will return the given
# <retcode>, and if the <retry-after> is non-negative, it will set the
# Retry-After head to that value.
#
# Subsequent calls will return a 302 redirect to <path>.
#
# Supported error codes are 429,502,503, and 504

print_status() {
	if [ "$1" -eq "302" ]; then
		printf "Status: 302 Found\n"
	elif [ "$1" -eq "429" ]; then
		printf "Status: 429 Too Many Requests\n"
	elif [ "$1" -eq "502" ]; then
		printf "Status: 502 Bad Gateway\n"
	elif [ "$1" -eq "503" ]; then
		printf "Status: 503 Service Unavailable\n"
	elif [ "$1" -eq "504" ]; then
		printf "Status: 504 Gateway Timeout\n"
	else
		printf "Status: 500 Internal Server Error\n"
	fi
	printf "Content-Type: text/plain\n"
}

# set $@ to components of PATH_INFO
IFS='/'
set -f
set -- $PATH_INFO
set +f

# pull out first four path components
shift
nonce=$1
times=$2
code=$3
retry=$4
shift 4

# glue the rest back together as redirect path
path=""
while [ "$#" -gt "0" ]; do
	path="${path}/$1"
	shift
done

# leave a cookie for this request/retry count
state_file="request_${REMOTE_ADDR}_${nonce}_${times}_${code}_${retry}"

if [ ! -f "$state_file" ]; then
	echo 0 > "$state_file"
fi

read -r cnt < "$state_file"
if [ "$cnt" -lt "$times" ]; then
	echo $((cnt+1)) > "$state_file"

	# return error
	print_status "$code"
	if [ "$retry" -ge "0" ]; then
		printf "Retry-After: %s\n" "$retry"
	fi
else
	# redirect
	print_status 302
	printf "Location: %s?%s\n" "$path" "${QUERY_STRING}"
fi

echo
