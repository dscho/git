#include "builtin.h"
#include "config.h"
#include "parse-options.h"
#include "fsmonitor.h"
#include "fsmonitor-ipc.h"
#include "compat/fsmonitor/fsmonitor-fs-listen.h"
#include "fsmonitor--daemon.h"
#include "simple-ipc.h"
#include "khash.h"

static const char * const builtin_fsmonitor__daemon_usage[] = {
	N_("git fsmonitor--daemon start [<options>]"),
	N_("git fsmonitor--daemon run [<options>]"),
	N_("git fsmonitor--daemon stop"),
	N_("git fsmonitor--daemon status"),
	NULL
};

#ifdef HAVE_FSMONITOR_DAEMON_BACKEND
/*
 * Global state loaded from config.
 */
#define FSMONITOR__IPC_THREADS "fsmonitor.ipcthreads"
static int fsmonitor__ipc_threads = 8;

#define FSMONITOR__START_TIMEOUT "fsmonitor.starttimeout"
static int fsmonitor__start_timeout_sec = 60;

static int fsmonitor_config(const char *var, const char *value, void *cb)
{
	if (!strcmp(var, FSMONITOR__IPC_THREADS)) {
		int i = git_config_int(var, value);
		if (i < 1)
			return error(_("value of '%s' out of range: %d"),
				     FSMONITOR__IPC_THREADS, i);
		fsmonitor__ipc_threads = i;
		return 0;
	}

	if (!strcmp(var, FSMONITOR__START_TIMEOUT)) {
		int i = git_config_int(var, value);
		if (i < 0)
			return error(_("value of '%s' out of range: %d"),
				     FSMONITOR__START_TIMEOUT, i);
		fsmonitor__start_timeout_sec = i;
		return 0;
	}

	return git_default_config(var, value, cb);
}

/*
 * Acting as a CLIENT.
 *
 * Send a "quit" command to the `git-fsmonitor--daemon` (if running)
 * and wait for it to shutdown.
 */
static int do_as_client__send_stop(void)
{
	struct strbuf answer = STRBUF_INIT;
	int ret;

	ret = fsmonitor_ipc__send_command("quit", &answer);

	/* The quit command does not return any response data. */
	strbuf_release(&answer);

	if (ret)
		return ret;

	trace2_region_enter("fsm_client", "polling-for-daemon-exit", NULL);
	while (fsmonitor_ipc__get_state() == IPC_STATE__LISTENING)
		sleep_millisec(50);
	trace2_region_leave("fsm_client", "polling-for-daemon-exit", NULL);

	return 0;
}

static int do_as_client__status(void)
{
	enum ipc_active_state state = fsmonitor_ipc__get_state();

	switch (state) {
	case IPC_STATE__LISTENING:
		printf(_("The built-in file system monitor is active\n"));
		return 0;

	default:
		printf(_("The built-in file system monitor is not active\n"));
		return 1;
	}
}

static ipc_server_application_cb handle_client;

static int handle_client(void *data,
			 const char *command, size_t command_len,
			 ipc_server_reply_cb *reply,
			 struct ipc_server_reply_data *reply_data)
{
	/* struct fsmonitor_daemon_state *state = data; */
	int result;

	/*
	 * The Simple IPC API now supports {char*, len} arguments, but
	 * FSMonitor always uses proper null-terminated strings, so
	 * we can ignore the command_len argument.  (Trust, but verify.)
	 */
	if (command_len != strlen(command))
		BUG("FSMonitor assumes text messages");

	trace2_region_enter("fsmonitor", "handle_client", the_repository);
	trace2_data_string("fsmonitor", the_repository, "request", command);

	result = 0; /* TODO Do something here. */

	trace2_region_leave("fsmonitor", "handle_client", the_repository);

	return result;
}

static void *fsmonitor_fs_listen__thread_proc(void *_state)
{
	struct fsmonitor_daemon_state *state = _state;

	trace2_thread_start("fsm-listen");

	trace_printf_key(&trace_fsmonitor, "Watching: worktree '%s'",
			 state->path_worktree_watch.buf);
	if (state->nr_paths_watching > 1)
		trace_printf_key(&trace_fsmonitor, "Watching: gitdir '%s'",
				 state->path_gitdir_watch.buf);

	fsmonitor_fs_listen__loop(state);

	trace2_thread_exit();
	return NULL;
}

static int fsmonitor_run_daemon_1(struct fsmonitor_daemon_state *state)
{
	struct ipc_server_opts ipc_opts = {
		.nr_threads = fsmonitor__ipc_threads,

		/*
		 * We know that there are no other active threads yet,
		 * so we can let the IPC layer temporarily chdir() if
		 * it needs to when creating the server side of the
		 * Unix domain socket.
		 */
		.uds_disallow_chdir = 0
	};

	/*
	 * Start the IPC thread pool before the we've started the file
	 * system event listener thread so that we have the IPC handle
	 * before we need it.
	 */
	if (ipc_server_run_async(&state->ipc_server_data,
				 fsmonitor_ipc__get_path(), &ipc_opts,
				 handle_client, state))
		return error(_("could not start IPC thread pool"));

	/*
	 * Start the fsmonitor listener thread to collect filesystem
	 * events.
	 */
	if (pthread_create(&state->listener_thread, NULL,
			   fsmonitor_fs_listen__thread_proc, state) < 0) {
		ipc_server_stop_async(state->ipc_server_data);
		ipc_server_await(state->ipc_server_data);

		return error(_("could not start fsmonitor listener thread"));
	}

	/*
	 * The daemon is now fully functional in background threads.
	 * Wait for the IPC thread pool to shutdown (whether by client
	 * request or from filesystem activity).
	 */
	ipc_server_await(state->ipc_server_data);

	/*
	 * The fsmonitor listener thread may have received a shutdown
	 * event from the IPC thread pool, but it doesn't hurt to tell
	 * it again.  And wait for it to shutdown.
	 */
	fsmonitor_fs_listen__stop_async(state);
	pthread_join(state->listener_thread, NULL);

	return state->error_code;
}

static int fsmonitor_run_daemon(void)
{
	struct fsmonitor_daemon_state state;
	int err;

	memset(&state, 0, sizeof(state));

	pthread_mutex_init(&state.main_lock, NULL);
	state.error_code = 0;
	state.current_token_data = NULL;

	/* Prepare to (recursively) watch the <worktree-root> directory. */
	strbuf_init(&state.path_worktree_watch, 0);
	strbuf_addstr(&state.path_worktree_watch, absolute_path(get_git_work_tree()));
	state.nr_paths_watching = 1;

	/*
	 * We create/delete cookie files inside the .git directory to
	 * help us keep sync with the file system.  If ".git" is not a
	 * directory, then <gitdir> is not inside the cone of
	 * <worktree-root>, so set up a second watch for it.
	 */
	strbuf_init(&state.path_gitdir_watch, 0);
	strbuf_addbuf(&state.path_gitdir_watch, &state.path_worktree_watch);
	strbuf_addstr(&state.path_gitdir_watch, "/.git");
	if (!is_directory(state.path_gitdir_watch.buf)) {
		strbuf_reset(&state.path_gitdir_watch);
		strbuf_addstr(&state.path_gitdir_watch, absolute_path(get_git_dir()));
		state.nr_paths_watching = 2;
	}

	/*
	 * Confirm that we can create platform-specific resources for the
	 * filesystem listener before we bother starting all the threads.
	 */
	if (fsmonitor_fs_listen__ctor(&state)) {
		err = error(_("could not initialize listener thread"));
		goto done;
	}

	err = fsmonitor_run_daemon_1(&state);

done:
	pthread_mutex_destroy(&state.main_lock);
	fsmonitor_fs_listen__dtor(&state);

	ipc_server_free(state.ipc_server_data);

	strbuf_release(&state.path_worktree_watch);
	strbuf_release(&state.path_gitdir_watch);

	return err;
}

static int try_to_run_foreground_daemon(void)
{
	/*
	 * Technically, we don't need to probe for an existing daemon
	 * process, since we could just call `fsmonitor_run_daemon()`
	 * and let it fail if the pipe/socket is busy.
	 *
	 * However, this method gives us a nicer error message for a
	 * common error case.
	 */
	if (fsmonitor_ipc__get_state() == IPC_STATE__LISTENING)
		die("fsmonitor--daemon is already running.");

	return !!fsmonitor_run_daemon();
}

#ifndef GIT_WINDOWS_NATIVE
/*
 * This is adapted from `daemonize()`.  Use `fork()` to directly create
 * and run the daemon in a child process.  The fork-parent returns the
 * child PID so that we can wait for the child to startup before exiting.
 */
static int spawn_background_fsmonitor_daemon(pid_t *pid)
{
	*pid = fork();

	switch (*pid) {
	case 0:
		if (setsid() == -1)
			error_errno(_("setsid failed"));
		close(0);
		close(1);
		close(2);
		sanitize_stdfds();

		return !!fsmonitor_run_daemon();

	case -1:
		return error_errno(_("could not spawn fsmonitor--daemon in the background"));

	default:
		return 0;
	}
}
#else
/*
 * Conceptually like `daemonize()` but different because Windows does not
 * have `fork(2)`.  Spawn a normal Windows child process but without the
 * limitations of `start_command()` and `finish_command()`.
 */
static int spawn_background_fsmonitor_daemon(pid_t *pid)
{
	char git_exe[MAX_PATH];
	struct strvec args = STRVEC_INIT;
	int in, out;

	GetModuleFileNameA(NULL, git_exe, MAX_PATH);

	in = open("/dev/null", O_RDONLY);
	out = open("/dev/null", O_WRONLY);

	strvec_push(&args, git_exe);
	strvec_push(&args, "fsmonitor--daemon");
	strvec_push(&args, "run");

	*pid = mingw_spawnvpe(args.v[0], args.v, NULL, NULL, in, out, out);
	close(in);
	close(out);

	strvec_clear(&args);

	if (*pid < 0)
		return error(_("could not spawn fsmonitor--daemon in the background"));

	return 0;
}
#endif

/*
 * This is adapted from `wait_or_whine()`.  Watch the child process and
 * let it get started and begin listening for requests on the socket
 * before reporting our success.
 */
static int wait_for_background_startup(pid_t pid_child)
{
	int status;
	pid_t pid_seen;
	enum ipc_active_state s;
	time_t time_limit, now;

	time(&time_limit);
	time_limit += fsmonitor__start_timeout_sec;

	for (;;) {
		pid_seen = waitpid(pid_child, &status, WNOHANG);

		if (pid_seen == -1)
			return error_errno(_("waitpid failed"));
		else if (pid_seen == 0) {
			/*
			 * The child is still running (this should be
			 * the normal case).  Try to connect to it on
			 * the socket and see if it is ready for
			 * business.
			 *
			 * If there is another daemon already running,
			 * our child will fail to start (possibly
			 * after a timeout on the lock), but we don't
			 * care (who responds) if the socket is live.
			 */
			s = fsmonitor_ipc__get_state();
			if (s == IPC_STATE__LISTENING)
				return 0;

			time(&now);
			if (now > time_limit)
				return error(_("fsmonitor--daemon not online yet"));
		} else if (pid_seen == pid_child) {
			/*
			 * The new child daemon process shutdown while
			 * it was starting up, so it is not listening
			 * on the socket.
			 *
			 * Try to ping the socket in the odd chance
			 * that another daemon started (or was already
			 * running) while our child was starting.
			 *
			 * Again, we don't care who services the socket.
			 */
			s = fsmonitor_ipc__get_state();
			if (s == IPC_STATE__LISTENING)
				return 0;

			/*
			 * We don't care about the WEXITSTATUS() nor
			 * any of the WIF*(status) values because
			 * `cmd_fsmonitor__daemon()` does the `!!result`
			 * trick on all function return values.
			 *
			 * So it is sufficient to just report the
			 * early shutdown as an error.
			 */
			return error(_("fsmonitor--daemon failed to start"));
		} else
			return error(_("waitpid is confused"));
	}
}

static int try_to_start_background_daemon(void)
{
	pid_t pid_child;
	int ret;

	/*
	 * Before we try to create a background daemon process, see
	 * if a daemon process is already listening.  This makes it
	 * easier for us to report an already-listening error to the
	 * console, since our spawn/daemon can only report the success
	 * of creating the background process (and not whether it
	 * immediately exited).
	 */
	if (fsmonitor_ipc__get_state() == IPC_STATE__LISTENING)
		die("fsmonitor--daemon is already running.");

	/*
	 * Run the actual daemon in a background process.
	 */
	ret = spawn_background_fsmonitor_daemon(&pid_child);
	if (pid_child <= 0)
		return ret;

	/*
	 * Wait (with timeout) for the background child process get
	 * started and begin listening on the socket/pipe.  This makes
	 * the "start" command more synchronous and more reliable in
	 * tests.
	 */
	ret = wait_for_background_startup(pid_child);

	return ret;
}

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	const char *subcmd;

	struct option options[] = {
		OPT_INTEGER(0, "ipc-threads",
			    &fsmonitor__ipc_threads,
			    N_("use <n> ipc worker threads")),
		OPT_INTEGER(0, "start-timeout",
			    &fsmonitor__start_timeout_sec,
			    N_("Max seconds to wait for background daemon startup")),

		OPT_END()
	};

	if (argc < 2)
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	git_config(fsmonitor_config, NULL);

	subcmd = argv[1];
	argv--;
	argc++;

	argc = parse_options(argc, argv, prefix, options,
			     builtin_fsmonitor__daemon_usage, 0);
	if (fsmonitor__ipc_threads < 1)
		die(_("invalid 'ipc-threads' value (%d)"),
		    fsmonitor__ipc_threads);

	if (!strcmp(subcmd, "start"))
		return !!try_to_start_background_daemon();

	if (!strcmp(subcmd, "run"))
		return !!try_to_run_foreground_daemon();

	if (!strcmp(subcmd, "stop"))
		return !!do_as_client__send_stop();

	if (!strcmp(subcmd, "status"))
		return !!do_as_client__status();

	die(_("Unhandled subcommand '%s'"), subcmd);
}

#else
int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	struct option options[] = {
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	die(_("fsmonitor--daemon not supported on this platform"));
}
#endif
