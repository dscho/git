/*
 * Copyright (c) 2005, 2006 Rene Scharfe
 */
#include "cache.h"
#include "commit.h"
#include "tar.h"
#include "builtin.h"
#include "quote.h"
#include "config.h"

static const char builtin_get_tar_commit_id_usage[] =
"git get-tar-commit-id";

/* ustar header + extended global header content */
#define RECORDSIZE	(512)
#define HEADERSIZE (2 * RECORDSIZE)

int cmd_get_tar_commit_id(int argc, const char **argv, const char *prefix)
{
	char buffer[HEADERSIZE];
	struct ustar_header *header = (struct ustar_header *)buffer;
	char *content = buffer + RECORDSIZE;
	const char *comment;
	ssize_t n;

	if (argc != 1)
		usage(builtin_get_tar_commit_id_usage);

	git_config(git_default_config, NULL);
	n = read_in_full(0, buffer, HEADERSIZE);
	if (n < 0)
		die_errno("git get-tar-commit-id: read error");
	if (n != HEADERSIZE)
		die_errno("git get-tar-commit-id: EOF before reading tar header");
	if (header->typeflag[0] != 'g')
		return 1;
	if (!skip_prefix(content, "52 comment=", &comment))
		return 1;

	if (write_in_full(1, comment, 41) < 0)
		die_errno("git get-tar-commit-id: write error");

	return 0;
}
