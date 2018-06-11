#include "cache.h"
#include "hook.h"
#include "run-command.h"
#include "config.h"

static int early_hooks_path_config(const char *var, const char *value, void *data)
{
	if (!strcmp(var, "core.hookspath"))
		return git_config_pathname((const char **)data, var, value);

	return 0;
}

/* Discover the hook before setup_git_directory() was called */
static const char *hook_path_early(const char *name, struct strbuf *result)
{
	static struct strbuf hooks_dir = STRBUF_INIT;
	static int initialized;

	if (initialized < 0)
		return NULL;

	if (!initialized) {
		struct strbuf gitdir = STRBUF_INIT, commondir = STRBUF_INIT;
		const char *early_hooks_dir = NULL;

		if (discover_git_directory(&commondir, &gitdir) < 0) {
			initialized = -1;
			return NULL;
		}

		read_early_config(early_hooks_path_config, &early_hooks_dir);
		if (!early_hooks_dir)
			strbuf_addf(&hooks_dir, "%s/hooks/", commondir.buf);
		else {
			strbuf_add_absolute_path(&hooks_dir, early_hooks_dir);
			strbuf_addch(&hooks_dir, '/');
		}

		strbuf_release(&gitdir);
		strbuf_release(&commondir);

		initialized = 1;
	}

	strbuf_addf(result, "%s%s", hooks_dir.buf, name);
	return result->buf;
}

const char *find_hook(const char *name)
{
	static struct strbuf path = STRBUF_INIT;

	strbuf_reset(&path);
	if (have_git_dir()) {
		static int forced_config;

		if (!forced_config) {
			if (!git_hooks_path)
				git_config_get_pathname("core.hookspath",
							&git_hooks_path);
			forced_config = 1;
		}

		strbuf_git_path(&path, "hooks/%s", name);
	} else if (!hook_path_early(name, &path))
		return NULL;

	if (access(path.buf, X_OK) < 0) {
		int err = errno;

#ifdef STRIP_EXTENSION
		strbuf_addstr(&path, STRIP_EXTENSION);
		if (access(path.buf, X_OK) >= 0)
			return path.buf;
		if (errno == EACCES)
			err = errno;
#endif

		if (err == EACCES && advice_enabled(ADVICE_IGNORED_HOOK)) {
			static struct string_list advise_given = STRING_LIST_INIT_DUP;

			if (!string_list_lookup(&advise_given, name)) {
				string_list_insert(&advise_given, name);
				advise(_("The '%s' hook was ignored because "
					 "it's not set as executable.\n"
					 "You can disable this warning with "
					 "`git config advice.ignoredHook false`."),
				       path.buf);
			}
		}
		return NULL;
	}
	return path.buf;
}

int hook_exists(const char *name)
{
	return !!find_hook(name);
}
