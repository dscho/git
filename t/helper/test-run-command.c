/*
 * test-run-command.c: test run command API.
 *
 * (C) 2009 Ilari Liusvaara <ilari.liusvaara@elisanet.fi>
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "test-tool.h"
#include "git-compat-util.h"
#include "run-command.h"
#include "argv-array.h"
#include "strbuf.h"
#include <string.h>
#include <errno.h>

static int number_callbacks;
static int parallel_next(struct child_process *cp,
			 struct strbuf *err,
			 void *cb,
			 void **task_cb)
{
	struct child_process *d = cb;
	if (number_callbacks >= 4)
		return 0;

	argv_array_pushv(&cp->args, d->argv);
	strbuf_addstr(err, "preloaded output of a child\n");
	number_callbacks++;
	return 1;
}

static int no_job(struct child_process *cp,
		  struct strbuf *err,
		  void *cb,
		  void **task_cb)
{
	strbuf_addstr(err, "no further jobs available\n");
	return 0;
}

static int task_finished(int result,
			 struct strbuf *err,
			 void *pp_cb,
			 void *pp_task_cb)
{
	strbuf_addstr(err, "asking for a quick stop\n");
	return 1;
}

static int inherit_handle(const char *argv0)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	char path[PATH_MAX];
	int tmp;

	/* First, open an inheritable handle */
	xsnprintf(path, sizeof(path), "out-XXXXXX");
	tmp = xmkstemp(path);

	argv_array_pushl(&cp.args,
			 "test-tool", argv0, "inherited-handle-child", NULL);
	cp.in = -1;
	cp.no_stdout = cp.no_stderr = 1;
	if (start_command(&cp) < 0)
		die("Could not start child process");

	/* Then close it, and try to delete it. */
	close(tmp);
	if (unlink(path))
		die("Could not delete '%s'", path);

	if (close(cp.in) < 0 || finish_command(&cp) < 0)
		die("Child did not finish");

	return 0;
}

static int inherit_handle_child(void)
{
	struct strbuf buf = STRBUF_INIT;

	if (strbuf_read(&buf, 0, 0) < 0)
		die("Could not read stdin");
	printf("Received %s\n", buf.buf);
	strbuf_release(&buf);

	return 0;
}

int cmd__run_command(int argc, const char **argv)
{
	struct child_process proc = CHILD_PROCESS_INIT;
	int jobs;

	if (argc < 2)
		return 1;
	if (!strcmp(argv[1], "inherited-handle"))
		exit(inherit_handle(argv[0]));
	if (!strcmp(argv[1], "inherited-handle-child"))
		exit(inherit_handle_child());

	if (argc < 3)
		return 1;
	while (!strcmp(argv[1], "env")) {
		if (!argv[2])
			die("env specifier without a value");
		argv_array_push(&proc.env_array, argv[2]);
		argv += 2;
		argc -= 2;
	}
	if (argc < 3)
		return 1;
	proc.argv = (const char **)argv + 2;

	if (!strcmp(argv[1], "start-command-ENOENT")) {
		if (start_command(&proc) < 0 && errno == ENOENT)
			return 0;
		fprintf(stderr, "FAIL %s\n", argv[1]);
		return 1;
	}
	if (!strcmp(argv[1], "run-command"))
		exit(run_command(&proc));

	jobs = atoi(argv[2]);
	proc.argv = (const char **)argv + 3;

	if (!strcmp(argv[1], "run-command-parallel"))
		exit(run_processes_parallel(jobs, parallel_next,
					    NULL, NULL, &proc));

	if (!strcmp(argv[1], "run-command-abort"))
		exit(run_processes_parallel(jobs, parallel_next,
					    NULL, task_finished, &proc));

	if (!strcmp(argv[1], "run-command-no-jobs"))
		exit(run_processes_parallel(jobs, no_job,
					    NULL, task_finished, &proc));

	fprintf(stderr, "check usage\n");
	return 1;
}
