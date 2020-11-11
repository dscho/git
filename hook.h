#include "config.h"
#include "list.h"
#include "strbuf.h"
#include "strvec.h"

struct hook
{
	struct list_head list;
	/*
	 * Config file which holds the hook.*.command definition.
	 * (This has nothing to do with the hookcmd.<name>.* configs.)
	 */
	enum config_scope origin;
	/* The literal command to run. */
	struct strbuf command;
	int from_hookdir;
};

/*
 * Provides a linked list of 'struct hook' detailing commands which should run
 * in response to the 'hookname' event, in execution order.
 */
struct list_head* hook_list(const struct strbuf *hookname);

enum hookdir_opt
{
	hookdir_no,
	hookdir_warn,
	hookdir_interactive,
	hookdir_yes,
	hookdir_unknown,
};

/*
 * Provides the hookdir_opt specified in the config without consulting any
 * command line arguments.
 */
enum hookdir_opt configured_hookdir_opt(void);

/*
 * Returns 1 if any hooks are specified in the config or if a hook exists in the
 * hookdir. Typically, invoke hook_exsts() like:
 *   hook_exists(hookname, configured_hookdir_opt());
 * Like with run_hooks, if you take a --run-hookdir flag, reflect that
 * user-specified behavior here instead.
 */
int hook_exists(const char *hookname, enum hookdir_opt should_run_hookdir);
/*
 * Runs all hooks associated to the 'hookname' event in order. Each hook will be
 * passed 'env' and 'args'.
 */
int run_hooks(const char *const *env, const char *hookname,
	      const struct strvec *args, enum hookdir_opt run_hookdir);

/* Free memory associated with a 'struct hook' */
void free_hook(struct hook *ptr);
/* Empties the list at 'head', calling 'free_hook()' on each entry */
void clear_hook_list(struct list_head *head);
