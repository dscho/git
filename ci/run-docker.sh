#!/bin/sh
#
# Download and run Docker image to build and test Git
#

. ${0%/*}/lib.sh

case "$jobname" in
linux32)
	CI_CONTAINER="daald/ubuntu32:xenial"
	;;
linux-musl)
	CI_CONTAINER=alpine
	;;
*)
	exit 1
	;;
esac

docker pull "$CI_CONTAINER"

# Use the following command to debug the docker build locally:
# <host-user-id> must be 0 if podman is used as drop-in replacement for docker
# $ docker run -itv "${PWD}:/usr/src/git" --entrypoint /bin/sh "$CI_CONTAINER"
# root@container:/# export jobname=<jobname>
# root@container:/# /usr/src/git/ci/run-docker-build.sh <host-user-id>

docker run \
	--interactive \
	--env DEVELOPER \
	--env DEFAULT_TEST_TARGET \
	--env GIT_PROVE_OPTS \
	--env GIT_TEST_OPTS \
	--env GIT_TEST_CLONE_2GB \
	--env MAKEFLAGS \
	--env jobname \
	--volume "${PWD}:/usr/src/git" \
	"$CI_CONTAINER" \
	/usr/src/git/ci/run-docker-build.sh $(id -u $USER)

check_unignored_build_artifacts
