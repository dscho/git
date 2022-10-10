#include "builtin.h"
#include "lockfile.h"
#include "merge-strategies.h"
#include "run-command.h"

static const char *pgm;

static int merge_one_file_spawn(struct index_state *istate,
				const struct object_id *orig_blob,
				const struct object_id *our_blob,
				const struct object_id *their_blob, const char *path,
				unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
				void *data)
{
	char oids[3][GIT_MAX_HEXSZ + 1] = {{0}};
	char modes[3][10] = {{0}};
	const char *arguments[] = { pgm, oids[0], oids[1], oids[2],
				    path, modes[0], modes[1], modes[2], NULL };

	if (orig_blob) {
		oid_to_hex_r(oids[0], orig_blob);
		xsnprintf(modes[0], sizeof(modes[0]), "%06o", orig_mode);
	}

	if (our_blob) {
		oid_to_hex_r(oids[1], our_blob);
		xsnprintf(modes[1], sizeof(modes[1]), "%06o", our_mode);
	}

	if (their_blob) {
		oid_to_hex_r(oids[2], their_blob);
		xsnprintf(modes[2], sizeof(modes[2]), "%06o", their_mode);
	}

	return run_command_v_opt(arguments, 0);
}

int cmd_merge_index(int argc, const char **argv, const char *prefix)
{
	int i, force_file = 0, err = 0, one_shot = 0, quiet = 0;
	merge_fn merge_action = merge_one_file_spawn;
	struct lock_file lock = LOCK_INIT;
	struct repository *r = the_repository;
	const char *use_internal = NULL;

	/* Without this we cannot rely on waitpid() to tell
	 * what happened to our children.
	 */
	signal(SIGCHLD, SIG_DFL);

	if (argc < 3)
		usage("git merge-index [-o] [-q] (<merge-program> | --use=merge-one-file) (-a | [--] [<filename>...])");

	if (repo_read_index(r) < 0)
		die("invalid index");

	/* TODO: audit for interaction with sparse-index. */
	ensure_full_index(r->index);

	i = 1;
	if (!strcmp(argv[i], "-o")) {
		one_shot = 1;
		i++;
	}
	if (!strcmp(argv[i], "-q")) {
		quiet = 1;
		i++;
	}

	pgm = argv[i++];
	setup_work_tree();

	if (skip_prefix(pgm, "--use=", &use_internal)) {
		if (!strcmp(use_internal, "merge-one-file"))
			merge_action = merge_one_file_func;
		else
			die(_("git merge-index: unknown internal program %s"), use_internal);

		repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
	}

	for (; i < argc; i++) {
		const char *arg = argv[i];
		if (!force_file && *arg == '-') {
			if (!strcmp(arg, "--")) {
				force_file = 1;
				continue;
			}
			if (!strcmp(arg, "-a")) {
				err |= merge_all_index(r->index, one_shot, quiet,
						       merge_action, NULL);
				continue;
			}
			die("git merge-index: unknown option %s", arg);
		}
		err |= merge_index_path(r->index, one_shot, quiet, arg,
					merge_action, NULL);
	}

	if (is_lock_file_locked(&lock)) {
		if (err)
			rollback_lock_file(&lock);
		else
			return write_locked_index(r->index, &lock, COMMIT_LOCK);
	}

	return err;
}
