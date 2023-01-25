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

int cmd_subtree(int argc, const char **argv, const char *prefix)
{
	struct option options[] = {
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, options, subtree_usage, 0);

	die("TODO");
}
