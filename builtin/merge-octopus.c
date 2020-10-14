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

	if (argc < 5)
		usage(builtin_merge_octopus_usage);

	setup_work_tree();
	if (repo_read_index(the_repository) < 0)
		die("corrupted cache");

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

			get_oid(argv[i], &oid);

			if (!oideq(&oid, the_hash_algo->empty_tree)) {
				struct commit *commit;
				commit = lookup_commit_or_die(&oid, argv[i]);

				if (sep_seen)
					next_remote = commit_list_append(commit, next_remote);
				else
					next_base = commit_list_append(commit, next_base);
			}
		}
	}

	/*
	 * Reject if this is not an octopus -- resolve should be used
	 * instead.
	 */
	if (commit_list_count(remotes) < 2)
		return 2;

	return merge_strategies_octopus(the_repository, bases, head_arg, remotes);
}
