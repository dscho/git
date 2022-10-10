/*
 * Builtin "git merge-one-file"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-one-file.sh, written by Linus Torvalds.
 *
 * This is the git per-file merge utility, called with
 *
 *   argv[1] - original file object name (or empty)
 *   argv[2] - file in branch1 object name (or empty)
 *   argv[3] - file in branch2 object name (or empty)
 *   argv[4] - pathname in repository
 *   argv[5] - original file mode (or empty)
 *   argv[6] - file in branch1 mode (or empty)
 *   argv[7] - file in branch2 mode (or empty)
 *
 * Handle some trivial cases. The _really_ trivial cases have been
 * handled already by git read-tree, but that one doesn't do any merges
 * that might change the tree layout.
 */

#include "cache.h"
#include "builtin.h"
#include "lockfile.h"
#include "merge-strategies.h"

static const char builtin_merge_one_file_usage[] =
	"git merge-one-file <orig blob> <our blob> <their blob> <path> "
	"<orig mode> <our mode> <their mode>\n\n"
	"Blob ids and modes should be empty for missing files.";

static int read_param(const char *name, const char *arg_blob, const char *arg_mode,
		      struct object_id *blob, struct object_id **p_blob, unsigned int *mode)
{
	if (*arg_blob && !get_oid_hex(arg_blob, blob)) {
		char *last;

		*p_blob = blob;
		*mode = strtol(arg_mode, &last, 8);

		if (*last)
			return error(_("invalid '%s' mode: expected nothing, got '%c'"), name, *last);
		else if (!(S_ISREG(*mode) || S_ISDIR(*mode) || S_ISLNK(*mode)))
			return error(_("invalid '%s' mode: %o"), name, *mode);
	} else if (!*arg_blob && *arg_mode)
		return error(_("no '%s' object id given, but a mode was still given."), name);

	return 0;
}

int cmd_merge_one_file(int argc, const char **argv, const char *prefix)
{
	struct object_id orig_blob, our_blob, their_blob,
		*p_orig_blob = NULL, *p_our_blob = NULL, *p_their_blob = NULL;
	unsigned int orig_mode = 0, our_mode = 0, their_mode = 0, ret = 0;
	struct lock_file lock = LOCK_INIT;
	struct repository *r = the_repository;

	if (argc != 8)
		usage(builtin_merge_one_file_usage);

	if (repo_read_index(r) < 0)
		die("invalid index");

	repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);

	if (read_param("orig", argv[1], argv[5], &orig_blob,
		       &p_orig_blob, &orig_mode))
		ret = -1;

	if (read_param("our", argv[2], argv[6], &our_blob,
		       &p_our_blob, &our_mode))
		ret = -1;

	if (read_param("their", argv[3], argv[7], &their_blob,
		       &p_their_blob, &their_mode))
		ret = -1;

	if (ret)
		return ret;

	ret = merge_three_way(r->index, p_orig_blob, p_our_blob, p_their_blob,
			      argv[4], orig_mode, our_mode, their_mode);

	if (ret) {
		rollback_lock_file(&lock);
		return !!ret;
	}

	return write_locked_index(r->index, &lock, COMMIT_LOCK);
}
