#!/bin/sh

test_description='verify that push respects `pack.usePathWalk`'

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-pack.sh

test_expect_success 'setup bare repository and clone' '
	git init --bare -b main bare.git &&
	git --git-dir=bare.git config receive.unpackLimit 0 &&
	git --git-dir bare.git commit-tree -m initial $EMPTY_TREE >head_oid &&
	git --git-dir bare.git update-ref refs/heads/main $(cat head_oid) &&
	git clone --bare bare.git clone.git
'
test_expect_success 'avoid reusing deltified objects' '
	# construct two commits, one containing a file with the hex digits
	# repeated 16 times, the next reducing that to 8 times. The crucial
	# part is that the blob of the second commit is deltified _really_
	# badly and it is therefore easy to detect if a `git push` reused that
	# delta.
	x="0123456789abcdef" &&
	printf "$x$x$x$x$x$x$x$x" >x128 &&
	printf "$x$x$x$x$x$x$x$x$x$x$x$x$x$x$x$x" >x256 &&

	pack=clone.git/objects/pack/pack-tmp.pack &&
	pack_header 2 >$pack &&

	# add x256 as a non-deltified object, using uncompressed zlib for simplicity
	# 0x30 = OBJ_BLOB << 4, 0x80 = size larger than 15, 0x0 = lower 4 bits of size, 0x10 = bits 5-9 of size (size = 256)
	printf "\xb0\x10" >>$pack &&
	# Uncompressed zlib stream always starts with 0x78 0x01 0x01, followed
	# by two bytes encoding the size, little endian, then two bytes with
	# the bitwise-complement of that size, then the payload, and then the
	# Adler32 checksum. For some reason, the checksum is in big-endian format.
	printf "\x78\x01\x01\0\x01\xff\xfe" >>$pack &&
	cat x256 >>$pack &&
	# Manually-computed Adler32 checksum: 0xd7ae4621
	printf "\xd7\xae\x46\x21" >>$pack &&

	# add x128 as a very badly deltified object
	# 0x60 = OBJ_OFS_DELTA << 4, 0x80 = total size larger than 15, 0x4 = lower 4 bits of size, 0x03 = bits 5-9 of size (size = 128 * 3 + 2 + 2)
	printf "\xe4\x18" >>$pack &&
	# 0x010d = size (i.e. the relative negative offset) of the previous object (x256, used as base object)
	# encoded as 0x80 | ((0x010d >> 7) - 1), 0x010d & 0x7f
	printf "\x81\x0d" >>$pack &&
	# Uncompressed zlib stream, as before, size = 2 + 2 + 128 * 3 (i.e. 0x184)
	printf "\x78\x01\x01\x84\x01\x7b\xfe" >>$pack &&
	# base object size = 0x0100 (encoded as 0x80 | (0x0100 & 0x7f), 0x0100 >> 7
	printf "\x80\x02" >>$pack &&
	# object size = 0x80 (encoded as 0x80 | (0x80 & 0x7f), 0x80 >> 7
	printf "\x80\x01" >>$pack &&
	# massively badly-deltified object: copy every single byte individually
	# 0x80 = copy, 0x01 = use 1 byte to encode the offset (0), 0x10 = use 1 byte to encode the size (1, i.e. 0x01)
	printf "$(printf "\\\\x91\\\\x%02x\\\\x01" $(test_seq 0 127))" >>$pack &&
	# Manually-computed Adler32 checksum: 0x99c369c4
	printf "\x99\xc3\x69\xc4" >>$pack &&

	pack_trailer $pack &&
	git index-pack -v $pack &&

	oid256=$(git hash-object x256) &&
	printf "100755 blob $oid256\thex\n" >tree &&
	tree_oid="$(git --git-dir=clone.git mktree <tree)" &&
	commit_oid=$(git --git-dir=clone.git commit-tree \
		-p $(git --git-dir=clone.git rev-parse main) \
		-m 256 $tree_oid) &&

	oid128=$(git hash-object x128) &&
	printf "100755 blob $oid128\thex\n" >tree &&
	tree_oid="$(git --git-dir=clone.git mktree <tree)" &&
	commit_oid=$(git --git-dir=clone.git commit-tree \
		-p $commit_oid \
		-m 128 $tree_oid) &&

	# Verify that the on-disk size of the delta object is suboptimal in the
	# clone (see below why 18 bytes or smaller is the optimal size):
	git index-pack --verify-stat clone.git/objects/pack/pack-*.pack >verify &&
	size="$(sed -n "s/^$oid128 blob *\([^ ]*\).*/\1/p" <verify)" &&
	test $size -gt 18 &&

	git --git-dir=clone.git update-ref refs/heads/main $commit_oid &&
	git --git-dir=clone.git -c pack.usePathWalk=true push origin main &&
	git index-pack --verify-stat bare.git/objects/pack/pack-*.pack >verify &&
	size="$(sed -n "s/^$oid128 blob *\([^ ]*\).*/\1/p" <verify)" &&
	# The on-disk size of the delta object should be smaller than, or equal
	# to, 18 bytes, as that would be the size if storing the payload
	# uncompressed:
	#   3 bytes: \x78\x01\x01
	# + 2 bytes: zlib stream size
	# + 2 bytes: but-wise complement of the zlib stream size
	# + 7 bytes: payload
	#   (= 2 bytes for the size of tbe base object
	#    + 2 bytes for the size of the delta command
	#    + 3 bytes for the copy command)
	# + 2 + 2 bytes: Adler32 checksum
	test $size -le 18
'

test_done
