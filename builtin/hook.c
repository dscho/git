#include "cache.h"

#include "builtin.h"
#include "config.h"
#include "hook.h"
#include "parse-options.h"
#include "strbuf.h"
#include "strvec.h"

static const char * const builtin_hook_usage[] = {
	N_("git hook list <hookname>"),
	N_("git hook run [(-e|--env)=<var>...] [(-a|--arg)=<arg>...] <hookname>"),
	NULL
};

static enum hookdir_opt should_run_hookdir;

static int list(int argc, const char **argv, const char *prefix)
{
	struct list_head *head, *pos;
	struct hook *item;
	struct strbuf hookname = STRBUF_INIT;
	struct strbuf hookdir_annotation = STRBUF_INIT;

	struct option list_options[] = {
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, list_options,
			     builtin_hook_usage, 0);

	if (argc < 1) {
		usage_msg_opt(_("You must specify a hook event name to list."),
			      builtin_hook_usage, list_options);
	}

	strbuf_addstr(&hookname, argv[0]);

	head = hook_list(&hookname);

	if (list_empty(head)) {
		printf(_("no commands configured for hook '%s'\n"),
		       hookname.buf);
		strbuf_release(&hookname);
		return 0;
	}

	switch (should_run_hookdir) {
		case hookdir_no:
			strbuf_addstr(&hookdir_annotation, _(" (will not run)"));
			break;
		case hookdir_interactive:
			strbuf_addstr(&hookdir_annotation, _(" (will prompt)"));
			break;
		case hookdir_warn:
		case hookdir_unknown:
			strbuf_addstr(&hookdir_annotation, _(" (will warn)"));
			break;
		case hookdir_yes:
		/*
		 * The default behavior should agree with
		 * hook.c:configured_hookdir_opt().
		 */
		default:
			break;
	}

	list_for_each(pos, head) {
		item = list_entry(pos, struct hook, list);
		if (item) {
			/* Don't translate 'hookdir' - it matches the config */
			printf("%s: %s%s\n",
			       (item->from_hookdir
				? "hookdir"
				: config_scope_name(item->origin)),
			       item->command.buf,
			       (item->from_hookdir
				? hookdir_annotation.buf
				: ""));
		}
	}

	clear_hook_list(head);
	strbuf_release(&hookname);

	return 0;
}

static int run(int argc, const char **argv, const char *prefix)
{
	struct strbuf hookname = STRBUF_INIT;
	struct strvec envs = STRVEC_INIT;
	struct strvec args = STRVEC_INIT;

	struct option run_options[] = {
		OPT_STRVEC('e', "env", &envs, N_("var"),
			   N_("environment variables for hook to use")),
		OPT_STRVEC('a', "arg", &args, N_("args"),
			   N_("argument to pass to hook")),
		OPT_END(),
	};

	/*
	 * While it makes sense to list hooks out-of-repo, it doesn't make sense
	 * to execute them. Hooks usually want to look at repository artifacts.
	 */
	if (!have_git_dir())
		usage_msg_opt(_("You must be in a Git repo to execute hooks."),
			      builtin_hook_usage, run_options);

	argc = parse_options(argc, argv, prefix, run_options,
			     builtin_hook_usage, 0);

	if (argc < 1)
		usage_msg_opt(_("You must specify a hook event to run."),
			      builtin_hook_usage, run_options);

	strbuf_addstr(&hookname, argv[0]);

	return run_hooks(envs.v, hookname.buf, &args, should_run_hookdir);
}

int cmd_hook(int argc, const char **argv, const char *prefix)
{
	const char *run_hookdir = NULL;

	struct option builtin_hook_options[] = {
		OPT_STRING(0, "run-hookdir", &run_hookdir, N_("option"),
			   N_("what to do with hooks found in the hookdir")),
		OPT_END(),
	};

	argc = parse_options(argc, argv, prefix, builtin_hook_options,
			     builtin_hook_usage, 0);

	/* after the parse, we should have "<command> <hookname> <args...>" */
	if (argc < 2)
		usage_with_options(builtin_hook_usage, builtin_hook_options);


	/* argument > config */
	if (run_hookdir)
		if (!strcmp(run_hookdir, "no"))
			should_run_hookdir = hookdir_no;
		else if (!strcmp(run_hookdir, "yes"))
			should_run_hookdir = hookdir_yes;
		else if (!strcmp(run_hookdir, "warn"))
			should_run_hookdir = hookdir_warn;
		else if (!strcmp(run_hookdir, "interactive"))
			should_run_hookdir = hookdir_interactive;
		else
			die(_("'%s' is not a valid option for --run-hookdir "
			      "(yes, warn, interactive, no)"), run_hookdir);
	else
		should_run_hookdir = configured_hookdir_opt();

	if (!strcmp(argv[0], "list"))
		return list(argc, argv, prefix);
	if (!strcmp(argv[0], "run"))
		return run(argc, argv, prefix);

	usage_with_options(builtin_hook_usage, builtin_hook_options);
}
