/*
 * built-in fsmonitor daemon
 *
 * Monitor filesystem changes to update the Git index intelligently.
 *
 * Copyright (c) 2019 Johannes Schindelin
 */

#include "builtin.h"
#include "parse-options.h"
#include "fsmonitor.h"
#include "simple-ipc.h"
#include "khash.h"

static const char * const builtin_fsmonitor__daemon_usage[] = {
	N_("git fsmonitor--daemon [--query] <version> <timestamp>"),
	N_("git fsmonitor--daemon <command-mode> [<options>...]"),
	NULL
};

#ifndef HAVE_FSMONITOR_DAEMON_BACKEND
#define FSMONITOR_DAEMON_IS_SUPPORTED 0
#define FSMONITOR_VERSION 0l

static int fsmonitor_query_daemon(const char *unused_since,
				  struct strbuf *unused_answer)
{
	die(_("no native fsmonitor daemon available"));
}

static int fsmonitor_run_daemon(int unused_background)
{
	die(_("no native fsmonitor daemon available"));
}

static int fsmonitor_daemon_is_running(void)
{
	warning(_("no native fsmonitor daemon available"));
	return 0;
}

static int fsmonitor_stop_daemon(void)
{
	warning(_("no native fsmonitor daemon available"));
	return 0;
}
#else
#define FSMONITOR_DAEMON_IS_SUPPORTED 1

struct ipc_data {
	struct ipc_command_listener data;
	struct fsmonitor_daemon_state *state;
};

KHASH_INIT(str, const char *, int, 0, kh_str_hash_func, kh_str_hash_equal);

static int handle_client(struct ipc_command_listener *data, const char *command,
			 ipc_reply_fn_t reply, void *reply_data)
{
	struct fsmonitor_daemon_state *state = ((struct ipc_data *)data)->state;
	unsigned long version;
	uintmax_t since;
	char *p;
	struct fsmonitor_queue_item *queue;
	struct strbuf token = STRBUF_INIT;
	intmax_t count = 0, duplicates = 0;
	kh_str_t *shown;
	int hash_ret;

	trace2_data_string("fsmonitor", the_repository, "command", command);

	if (!strcmp(command, "quit")) {
		if (fsmonitor_listen_stop(state))
			error("Could not terminate watcher thread");
		sleep_millisec(50);
		return SIMPLE_IPC_QUIT;
	}

	trace2_region_enter("fsmonitor", "serve", the_repository);

	version = strtoul(command, &p, 10);
	if (version != FSMONITOR_VERSION) {
		error(_("fsmonitor: unhandled version (%lu, command: %s)"),
		      version, command);
error:
		pthread_mutex_lock(&state->queue_update_lock);
		strbuf_addf(&token, "%"PRIu64"", state->latest_update);
		pthread_mutex_unlock(&state->queue_update_lock);
		reply(reply_data, token.buf, token.len + 1);
		reply(reply_data, "/", 2);
		strbuf_release(&token);
		trace2_region_leave("fsmonitor", "serve", the_repository);
		return -1;
	}
	while (isspace(*p))
		p++;
	since = strtoumax(p, &p, 10);
	if (!since || since < state->latest_update || *p) {
		pthread_mutex_unlock(&state->queue_update_lock);
		error(_("fsmonitor: %s (%" PRIuMAX", command: %s, rest %s)"),
		      *p ? "extra stuff" : "incorrect/early timestamp",
		      since, command, p);
		goto error;
	}

	pthread_mutex_lock(&state->queue_update_lock);
	queue = state->first;
	strbuf_addf(&token, "%"PRIu64"", state->latest_update);
	pthread_mutex_unlock(&state->queue_update_lock);

	reply(reply_data, token.buf, token.len + 1);
	shown = kh_init_str();
	while (queue && queue->time >= since) {
		if (kh_get_str(shown, queue->path->path) != kh_end(shown))
			duplicates++;
		else {
			kh_put_str(shown, queue->path->path, &hash_ret);

			/* write the path, followed by a NUL */
			if (reply(reply_data,
				  queue->path->path, queue->path->len + 1) < 0)
				break;
			trace2_data_string("fsmonitor", the_repository,
					   "serve.path", queue->path->path);
			count++;
		}
		queue = queue->next;
	}

	kh_release_str(shown);
	strbuf_release(&token);
	trace2_data_intmax("fsmonitor", the_repository, "serve.count", count);
	trace2_data_intmax("fsmonitor", the_repository, "serve.skipped-duplicates", duplicates);
	trace2_region_leave("fsmonitor", "serve", the_repository);

	return 0;
}

static int paths_cmp(const void *data, const struct hashmap_entry *he1,
		     const struct hashmap_entry *he2, const void *keydata)
{
	const struct fsmonitor_path *a =
		container_of(he1, const struct fsmonitor_path, entry);
	const struct fsmonitor_path *b =
		container_of(he2, const struct fsmonitor_path, entry);

	return strcmp(a->path, keydata ? keydata : b->path);
}

int fsmonitor_special_path(struct fsmonitor_daemon_state *state,
			   const char *path, size_t len, int was_deleted)
{
	if (len < 4 || fspathncmp(path, ".git", 4) || (path[4] && path[4] != '/'))
		return 0;

	if (was_deleted && (len == 4 || len == 5))
		return FSMONITOR_DAEMON_QUIT;

	return 1;
}

int fsmonitor_queue_path(struct fsmonitor_daemon_state *state,
			 struct fsmonitor_queue_item **queue,
			 const char *path, size_t len, uint64_t time)
{
	struct fsmonitor_path lookup, *e;
	struct fsmonitor_queue_item *item;

	hashmap_entry_init(&lookup.entry, len);
	lookup.path = path;
	lookup.len = len;
	e = hashmap_get_entry(&state->paths, &lookup, entry, NULL);

	if (!e) {
		FLEXPTR_ALLOC_MEM(e, path, path, len);
		e->len = len;
		hashmap_put(&state->paths, &e->entry);
	}

	trace2_data_string("fsmonitor", the_repository, "path", e->path);

	item = xmalloc(sizeof(*item));
	item->path = e;
	item->time = time;
	item->previous = NULL;
	item->next = *queue;
	(*queue)->previous = item;
	*queue = item;

	return 0;
}

static int fsmonitor_run_daemon(int background)
{
	struct fsmonitor_daemon_state state = { { 0 } };
	struct ipc_data ipc_data = {
		.data = {
			.path = git_path_fsmonitor(),
			.handle_client = handle_client,
		},
		.state = &state,
	};

	if (background && daemonize())
		BUG(_("daemonize() not supported on this platform"));

	hashmap_init(&state.paths, paths_cmp, NULL, 0);
	pthread_mutex_init(&state.queue_update_lock, NULL);
	pthread_mutex_init(&state.initial_mutex, NULL);
	pthread_cond_init(&state.initial_cond, NULL);

	pthread_mutex_lock(&state.initial_mutex);
	if (pthread_create(&state.watcher_thread, NULL,
			   (void *(*)(void *)) fsmonitor_listen, &state) < 0)
		return error(_("could not start fsmonitor listener thread"));

	/* wait for the thread to signal that it is ready */
	while (!state.initialized)
		pthread_cond_wait(&state.initial_cond, &state.initial_mutex);
	pthread_mutex_unlock(&state.initial_mutex);

	return ipc_listen_for_commands(&ipc_data.data);
}
#endif

int cmd_fsmonitor__daemon(int argc, const char **argv, const char *prefix)
{
	enum daemon_mode {
		QUERY = 0, RUN, START, STOP, IS_RUNNING, IS_SUPPORTED
	} mode = QUERY;
	struct option options[] = {
		OPT_CMDMODE(0, "query", &mode, N_("query the daemon"), QUERY),
		OPT_CMDMODE(0, "run", &mode, N_("run the daemon"), RUN),
		OPT_CMDMODE(0, "start", &mode, N_("run in the background"),
			    START),
		OPT_CMDMODE(0, "stop", &mode, N_("stop the running daemon"),
			    STOP),
		OPT_CMDMODE('t', "is-running", &mode,
			    N_("test whether the daemon is running"),
			    IS_RUNNING),
		OPT_CMDMODE(0, "is-supported", &mode,
			    N_("determine internal fsmonitor on this platform"),
			    IS_SUPPORTED),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	argc = parse_options(argc, argv, prefix, options,
			     builtin_fsmonitor__daemon_usage, 0);

	if (mode == QUERY) {
		struct strbuf answer = STRBUF_INIT;
		int ret;
		unsigned long version;

		if (argc != 2)
			usage_with_options(builtin_fsmonitor__daemon_usage,
					   options);

		version = strtoul(argv[0], NULL, 10);
		if (version != FSMONITOR_VERSION)
			die(_("unhandled fsmonitor version %ld (!= %ld)"),
			      version, FSMONITOR_VERSION);

		ret = fsmonitor_query_daemon(argv[1], &answer);
		if (ret < 0)
			die(_("could not query fsmonitor daemon"));
		write_in_full(1, answer.buf, answer.len);
		strbuf_release(&answer);

		return 0;
	}

	if (argc != 0)
		usage_with_options(builtin_fsmonitor__daemon_usage, options);

	if (mode == IS_SUPPORTED)
		return !FSMONITOR_DAEMON_IS_SUPPORTED;

	if (mode == IS_RUNNING)
		return !fsmonitor_daemon_is_running();

	if (mode == STOP) {
		if (fsmonitor_stop_daemon() < 0)
			die("could not stop daemon");
		while (fsmonitor_daemon_is_running())
			sleep_millisec(50);
		return 0;
	}

	if (fsmonitor_daemon_is_running())
		die("fsmonitor daemon is already running.");

#ifdef GIT_WINDOWS_NATIVE
	/* Windows cannot daemonize(); emulate it */
	if (mode == START)
		return !!fsmonitor_spawn_daemon();
#endif

	return !!fsmonitor_run_daemon(mode == START);
}
