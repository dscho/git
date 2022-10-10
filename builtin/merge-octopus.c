/*
 * Builtin "git merge-octopus"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-octopus.sh, written by Junio C Hamano.
 *
 * Resolve two or more trees.
 */

#include "cache.h"
#include "builtin.h"
#include "commit.h"
#include "merge-strategies.h"

static const char builtin_merge_octopus_usage[] =
	"git merge-octopus [<bases>...] -- <head> <remote1> <remote2> [<remotes>...]";

int cmd_merge_octopus(int argc, const char **argv, const char *prefix)
{
	int i, sep_seen = 0;
	struct commit_list *bases = NULL, *remotes = NULL;
	struct commit_list **next_base = &bases, **next_remote = &remotes;
	const char *head_arg = NULL;
	struct repository *r = the_repository;

	if (argc < 5)
		usage(builtin_merge_octopus_usage);

	setup_work_tree();
	if (repo_read_index(r) < 0)
		die("invalid index");

	/*
	 * The first parameters up to -- are merge bases; the rest are
	 * heads.
	 */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			sep_seen = 1;
		else if (strcmp(argv[i], "-h") == 0)
			usage(builtin_merge_octopus_usage);
		else if (sep_seen && !head_arg)
			head_arg = argv[i];
		else {
			struct object_id oid;
			struct commit *commit;

			if (get_oid(argv[i], &oid))
				die("object %s not found.", argv[i]);

			commit = oideq(&oid, r->hash_algo->empty_tree) ?
				NULL : lookup_commit_or_die(&oid, argv[i]);

			if (sep_seen)
				next_remote = commit_list_append(commit, next_remote);
			else
				next_base = commit_list_append(commit, next_base);
		}
	}

	return merge_strategies_octopus(r, bases, head_arg, remotes);
}
