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
	int i, sep_seen = 0;
	const char *head = NULL;
	struct commit_list *bases = NULL, *remote = NULL;
	struct commit_list **next_base = &bases;
	struct repository *r = the_repository;

	if (argc < 5)
		usage(builtin_merge_resolve_usage);

	setup_work_tree();
	if (repo_read_index(r) < 0)
		die("invalid index");

	/*
	 * The first parameters up to -- are merge bases; the rest are
	 * heads.
	 */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--"))
			sep_seen = 1;
		else if (!strcmp(argv[i], "-h"))
			usage(builtin_merge_resolve_usage);
		else if (sep_seen && !head)
			head = argv[i];
		else {
			struct object_id oid;
			struct commit *commit;

			if (get_oid(argv[i], &oid))
				die("object %s not found.", argv[i]);

			commit = oideq(&oid, r->hash_algo->empty_tree) ?
				NULL : lookup_commit_or_die(&oid, argv[i]);

			if (sep_seen)
				commit_list_insert(commit, &remote);
			else
				next_base = commit_list_append(commit, next_base);
		}
	}

	return merge_strategies_resolve(r, bases, head, remote);
}
