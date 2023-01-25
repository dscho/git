#include "builtin.h"
#include "cache.h"
#include "config.h"
#include "parse-options.h"

static const char * const subtree_usage[] = {
	N_("git subtree add --prefix=<prefix> <commit>"),
	N_("git subtree add --prefix=<prefix> <repository> <ref>"),
	N_("git subtree merge --prefix=<prefix> <commit>"),
	N_("git subtree split --prefix=<prefix> [<commit>]"),
	N_("git subtree pull --prefix=<prefix> <repository> <ref>"),
	N_("git subtree push --prefix=<prefix> <repository> <refspec>"),
	NULL,
};

static int cmd_subtree_add(int argc, const char **argv, const char *prefix UNUSED)
{
	die("TODO");
}

static int cmd_subtree_merge(int argc, const char **argv, const char *prefix UNUSED)
{
	die("TODO");
}

static int cmd_subtree_pull(int argc, const char **argv, const char *prefix UNUSED)
{
	die("TODO");
}

static int cmd_subtree_push(int argc, const char **argv, const char *prefix UNUSED)
{
	die("TODO");
}

static int cmd_subtree_split(int argc, const char **argv, const char *prefix UNUSED)
{
	die("TODO");
}

int cmd_subtree(int argc, const char **argv, const char *prefix)
{
	int allow_add_merge = 0, allow_split = 0;
	int quiet = 0, debug = 0, rejoin = 0, split_ignore_joins = 0;
	const char *split_annotate = NULL;
	const char *split_branch = NULL;
	const char *split_onto = NULL;
	const char *sub_dir = NULL;
	int add_merge_squash = 0;
	const char *add_merge_message = NULL;
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT__QUIET(&quiet, N_("quiet")),
		OPT_BOOL('d', NULL, &debug, N_("show debug messages")),
		OPT_STRING('P', "prefix", &sub_dir, N_("subdir"),
			   N_("the name of the subdir to split out")),
		OPT_GROUP(N_("options for 'split' (also: 'push')")),
		OPT_STRING(0, "annotate", &split_annotate, N_("prefix"),
			   N_("add a prefix to commit message of new commits")),
		OPT_STRING('b', "branch", &split_branch, N_("branch"),
			   N_("create a new branch from the split subtree")),
		OPT_BOOL(0, "ignore-joins", &split_ignore_joins,
			 N_("ignore prior --rejoin commits")),
		OPT_STRING(0, "onto", &split_onto, N_("rev"),
			   N_("try connecting new tree to an existing one")),
		OPT_BOOL(0, "rejoin", &rejoin,
			 N_("merge the new branch back into HEAD")),
		OPT_GROUP(N_("options for 'add' and 'merge' (also: 'pull', "
			     "'split --rejoin', and 'push --rejoin')")),
		OPT_STRING('m', "message", &add_merge_message, N_("message"),
			   N_("use the given message as the commit message for "
			      "the merge commit")),
		OPT_BOOL(0, "squash", &add_merge_squash,
			 N_("merge subtree changes as a single commit")),
		OPT_SUBCOMMAND("add", &fn, cmd_subtree_add),
		OPT_SUBCOMMAND("merge", &fn, cmd_subtree_merge),
		OPT_SUBCOMMAND("pull", &fn, cmd_subtree_pull),
		OPT_SUBCOMMAND("push", &fn, cmd_subtree_push),
		OPT_SUBCOMMAND("split", &fn, cmd_subtree_split),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, subtree_usage, 0);

	if (fn == cmd_subtree_add || fn == cmd_subtree_merge ||
	    fn == cmd_subtree_push)
		allow_add_merge = 1;
	else {
		allow_split = 1;
		allow_add_merge = rejoin;
	}

	if (!allow_split &&
	    (split_annotate || rejoin || split_onto || split_ignore_joins))
		die(_("--annotate/--branch/--join/--onto/--ignore-joins is "
		      "incompatible with split/push"));

	if (!allow_add_merge &&
	     (add_merge_message || add_merge_squash))
		die(_("--message/--squash incompatible with split/push"));

	return fn(argc, argv, prefix);
}
