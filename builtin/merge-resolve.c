/*
 * Builtin "git merge-resolve"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-resolve.sh, written by Linus Torvalds and Junio C
 * Hamano.
 *
 * Resolve two trees, using enhanced multi-base read-tree.
 */

#include "cache.h"
#include "builtin.h"
#include "merge-strategies.h"

static const char builtin_merge_resolve_usage[] =
	"git merge-resolve <bases>... -- <head> <remote>";

int cmd_merge_resolve(int argc, const char **argv, const char *prefix)
{
	int i, is_baseless = 1, sep_seen = 0;
	const char *head = NULL;
	struct commit_list *bases = NULL, *remote = NULL;
	struct commit_list **next_base = &bases;

	if (argc < 5)
		usage(builtin_merge_resolve_usage);

	setup_work_tree();
	if (repo_read_index(the_repository) < 0)
		die("invalid index");

	/* The first parameters up to -- are merge bases; the rest are
	 * heads. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			sep_seen = 1;
		else if (strcmp(argv[i], "-h") == 0)
			usage(builtin_merge_resolve_usage);
		else if (sep_seen && !head)
			head = argv[i];
		else if (remote) {
			/* Give up if we are given two or more remotes.
			 * Not handling octopus. */
			return 2;
		} else {
			struct object_id oid;

			get_oid(argv[i], &oid);
			is_baseless &= sep_seen;

			if (!oideq(&oid, the_hash_algo->empty_tree)) {
				struct commit *commit;
				commit = lookup_commit_or_die(&oid, argv[i]);

				if (sep_seen)
					commit_list_append(commit, &remote);
				else
					next_base = commit_list_append(commit, next_base);
			}
		}
	}

	/* Give up if this is a baseless merge. */
	if (is_baseless)
		return 2;

	return merge_strategies_resolve(the_repository, bases, head, remote);
}
