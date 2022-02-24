#include "cache.h"
#include "config.h"
#include "repository.h"
#include "fsmonitor-settings.h"

/*
 * We keep this structure defintion private and have getters
 * for all fields so that we can lazy load it as needed.
 */
struct fsmonitor_settings {
	enum fsmonitor_mode mode;
	enum fsmonitor_reason reason;
	char *hook_path;
};

static void set_incompatible(struct repository *r,
			     enum fsmonitor_reason reason)
{
	struct fsmonitor_settings *s = r->settings.fsmonitor;

	s->mode = FSMONITOR_MODE_INCOMPATIBLE;
	s->reason = reason;
}

static int check_for_incompatible(struct repository *r)
{
	if (!r->worktree) {
		/*
		 * Bare repositories don't have a working directory and
		 * therefore have nothing to watch.
		 */
		set_incompatible(r, FSMONITOR_REASON_BARE);
		return 1;
	}

#ifdef HAVE_FSMONITOR_OS_SETTINGS
	{
		enum fsmonitor_reason reason;

		reason = fsm_os__incompatible(r);
		if (reason != FSMONITOR_REASON_ZERO) {
			set_incompatible(r, reason);
			return 1;
		}
	}
#endif

	return 0;
}

static void lookup_fsmonitor_settings(struct repository *r)
{
	struct fsmonitor_settings *s;
	const char *const_str;
	int bool_value;

	if (r->settings.fsmonitor)
		return;

	CALLOC_ARRAY(s, 1);

	r->settings.fsmonitor = s;

	fsm_settings__set_disabled(r);

	/*
	 * Overload the existing "core.fsmonitor" config setting (which
	 * has historically been either unset or a hook pathname) to
	 * now allow a boolean value to enable the builtin FSMonitor
	 * or to turn everything off.  (This does imply that you can't
	 * use a hook script named "true" or "false", but that's OK.)
	 */
	switch (repo_config_get_maybe_bool(r, "core.fsmonitor", &bool_value)) {

	case 0: /* config value was set to <bool> */
		if (bool_value)
			fsm_settings__set_ipc(r);
		return;

	case 1: /* config value was unset */
		const_str = getenv("GIT_TEST_FSMONITOR");
		break;

	case -1: /* config value set to an arbitrary string */
		if (repo_config_get_pathname(r, "core.fsmonitor", &const_str))
			return; /* should not happen */
		break;

	default: /* should not happen */
		return;
	}

	if (!const_str || !*const_str)
		return;

	fsm_settings__set_hook(r, const_str);
}

enum fsmonitor_mode fsm_settings__get_mode(struct repository *r)
{
	lookup_fsmonitor_settings(r);

	return r->settings.fsmonitor->mode;
}

const char *fsm_settings__get_hook_path(struct repository *r)
{
	lookup_fsmonitor_settings(r);

	return r->settings.fsmonitor->hook_path;
}

void fsm_settings__set_ipc(struct repository *r)
{
	lookup_fsmonitor_settings(r);

	if (check_for_incompatible(r))
		return;

	r->settings.fsmonitor->mode = FSMONITOR_MODE_IPC;
	FREE_AND_NULL(r->settings.fsmonitor->hook_path);
}

void fsm_settings__set_hook(struct repository *r, const char *path)
{
	lookup_fsmonitor_settings(r);

	if (check_for_incompatible(r))
		return;

	r->settings.fsmonitor->mode = FSMONITOR_MODE_HOOK;
	FREE_AND_NULL(r->settings.fsmonitor->hook_path);
	r->settings.fsmonitor->hook_path = strdup(path);
}

void fsm_settings__set_disabled(struct repository *r)
{
	lookup_fsmonitor_settings(r);

	r->settings.fsmonitor->mode = FSMONITOR_MODE_DISABLED;
	r->settings.fsmonitor->reason = FSMONITOR_REASON_ZERO;
	FREE_AND_NULL(r->settings.fsmonitor->hook_path);
}

static void create_reason_message(struct repository *r,
				  struct strbuf *buf_reason)
{
	struct fsmonitor_settings *s = r->settings.fsmonitor;

	switch (s->reason) {
	case FSMONITOR_REASON_ZERO:
		return;

	case FSMONITOR_REASON_BARE:
		strbuf_addstr(buf_reason,
			      _("bare repos are incompatible with fsmonitor"));
		return;

	case FSMONITOR_REASON_VIRTUAL:
		strbuf_addstr(buf_reason,
			      _("virtual repos are incompatible with fsmonitor"));
		return;

	case FSMONITOR_REASON_REMOTE:
		strbuf_addstr(buf_reason,
			      _("remote repos are incompatible with fsmonitor"));
		return;

	default:
		BUG("Unhandled case in create_reason_message '%d'", s->reason);
	}
}

enum fsmonitor_reason fsm_settings__get_reason(struct repository *r,
					       struct strbuf *buf_reason)
{
	lookup_fsmonitor_settings(r);

	strbuf_reset(buf_reason);
	if (r->settings.fsmonitor->mode == FSMONITOR_MODE_INCOMPATIBLE)
		create_reason_message(r, buf_reason);

	return r->settings.fsmonitor->reason;
}
