#include "cache.h"
#include "cache-tree.h"
#include "commit-reach.h"
#include "dir.h"
#include "entry.h"
#include "ll-merge.h"
#include "lockfile.h"
#include "merge-strategies.h"
#include "run-command.h"
#include "unpack-trees.h"
#include "xdiff-interface.h"

static int add_to_index_cacheinfo(struct index_state *istate,
				  unsigned int mode,
				  const struct object_id *oid, const char *path)
{
	struct cache_entry *ce;
	int len, option;

	if (!verify_path(path, mode))
		return error(_("Invalid path '%s'"), path);

	len = strlen(path);
	ce = make_empty_cache_entry(istate, len);

	oidcpy(&ce->oid, oid);
	memcpy(ce->name, path, len);
	ce->ce_flags = create_ce_flags(0);
	ce->ce_namelen = len;
	ce->ce_mode = create_ce_mode(mode);
	if (assume_unchanged)
		ce->ce_flags |= CE_VALID;
	option = ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE;
	if (add_index_entry(istate, ce, option))
		return error(_("%s: cannot add to the index"), path);

	return 0;
}

static int checkout_from_index(struct index_state *istate, const char *path)
{
	struct checkout state = CHECKOUT_INIT;
	struct cache_entry *ce;

	state.istate = istate;
	state.force = 1;
	state.base_dir = "";
	state.base_dir_len = 0;

	ce = index_file_exists(istate, path, strlen(path), 0);
	if (checkout_entry(ce, &state, NULL, NULL) < 0)
		return error(_("%s: cannot checkout file"), path);
	return 0;
}

static int merge_one_file_deleted(struct index_state *istate,
				  const struct object_id *orig_blob,
				  const struct object_id *our_blob,
				  const struct object_id *their_blob, const char *path,
				  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if ((our_blob && orig_mode != our_mode) ||
	    (their_blob && orig_mode != their_mode))
		return error(_("File %s deleted on one branch but had its "
			       "permissions changed on the other."), path);

	if (our_blob) {
		printf(_("Removing %s\n"), path);

		if (file_exists(path))
			remove_path(path);
	}

	if (remove_file_from_index(istate, path))
		return error("%s: cannot remove from the index", path);
	return 0;
}

static int do_merge_one_file(struct index_state *istate,
			     const struct object_id *orig_blob,
			     const struct object_id *our_blob,
			     const struct object_id *their_blob, const char *path,
			     unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	int ret, i, dest;
	ssize_t written;
	mmbuffer_t result = {NULL, 0};
	mmfile_t mmfs[3];
	struct ll_merge_options merge_opts = {0};
	struct cache_entry *ce;

	if (our_mode == S_IFLNK || their_mode == S_IFLNK)
		return error(_("%s: Not merging symbolic link changes."), path);
	else if (our_mode == S_IFGITLINK || their_mode == S_IFGITLINK)
		return error(_("%s: Not merging conflicting submodule changes."), path);

	read_mmblob(mmfs + 1, our_blob);
	read_mmblob(mmfs + 2, their_blob);

	if (orig_blob) {
		printf(_("Auto-merging %s\n"), path);
		read_mmblob(mmfs + 0, orig_blob);
	} else {
		printf(_("Added %s in both, but differently.\n"), path);
		read_mmblob(mmfs + 0, &null_oid);
	}

	merge_opts.xdl_opts = XDL_MERGE_ZEALOUS_ALNUM;
	ret = ll_merge(&result, path,
		       mmfs + 0, "orig",
		       mmfs + 1, "our",
		       mmfs + 2, "their",
		       istate, &merge_opts);

	for (i = 0; i < 3; i++)
		free(mmfs[i].ptr);

	if (ret < 0) {
		free(result.ptr);
		return error(_("Failed to execute internal merge"));
	}

	/*
	 * Create the working tree file, using "our tree" version from
	 * the index, and then store the result of the merge.
	 */
	ce = index_file_exists(istate, path, strlen(path), 0);
	if (!ce)
		BUG("file is not present in the cache?");

	unlink(path);
	if ((dest = open(path, O_WRONLY | O_CREAT, ce->ce_mode)) < 0) {
		free(result.ptr);
		return error_errno(_("failed to open file '%s'"), path);
	}

	written = write_in_full(dest, result.ptr, result.size);
	close(dest);

	free(result.ptr);

	if (written < 0)
		return error_errno(_("failed to write to '%s'"), path);

	if (ret != 0 || !orig_blob)
		ret = error(_("content conflict in %s"), path);
	if (our_mode != their_mode)
		return error(_("permission conflict: %o->%o,%o in %s"),
			     orig_mode, our_mode, their_mode, path);
	if (ret)
		return -1;

	return add_file_to_index(istate, path, 0);
}

int merge_strategies_one_file(struct repository *r,
			      const struct object_id *orig_blob,
			      const struct object_id *our_blob,
			      const struct object_id *their_blob, const char *path,
			      unsigned int orig_mode, unsigned int our_mode,
			      unsigned int their_mode)
{
	if (orig_blob &&
	    ((!their_blob && our_blob && oideq(orig_blob, our_blob)) ||
	     (!our_blob && their_blob && oideq(orig_blob, their_blob))))
		/* Deleted in both or deleted in one and unchanged in the other. */
		return merge_one_file_deleted(r->index,
					      orig_blob, our_blob, their_blob, path,
					      orig_mode, our_mode, their_mode);
	else if (!orig_blob && our_blob && !their_blob) {
		/*
		 * Added in one.  The other side did not add and we
		 * added so there is nothing to be done, except making
		 * the path merged.
		 */
		return add_to_index_cacheinfo(r->index, our_mode, our_blob, path);
	} else if (!orig_blob && !our_blob && their_blob) {
		printf(_("Adding %s\n"), path);

		if (file_exists(path))
			return error(_("untracked %s is overwritten by the merge."), path);

		if (add_to_index_cacheinfo(r->index, their_mode, their_blob, path))
			return -1;
		return checkout_from_index(r->index, path);
	} else if (!orig_blob && our_blob && their_blob &&
		   oideq(our_blob, their_blob)) {
		/* Added in both, identically (check for same permissions). */
		if (our_mode != their_mode)
			return error(_("File %s added identically in both branches, "
				       "but permissions conflict %o->%o."),
				     path, our_mode, their_mode);

		printf(_("Adding %s\n"), path);

		if (add_to_index_cacheinfo(r->index, our_mode, our_blob, path))
			return -1;
		return checkout_from_index(r->index, path);
	} else if (our_blob && their_blob)
		/* Modified in both, but differently. */
		return do_merge_one_file(r->index,
					 orig_blob, our_blob, their_blob, path,
					 orig_mode, our_mode, their_mode);
	else {
		char orig_hex[GIT_MAX_HEXSZ] = {0}, our_hex[GIT_MAX_HEXSZ] = {0},
			their_hex[GIT_MAX_HEXSZ] = {0};

		if (orig_blob)
			oid_to_hex_r(orig_hex, orig_blob);
		if (our_blob)
			oid_to_hex_r(our_hex, our_blob);
		if (their_blob)
			oid_to_hex_r(their_hex, their_blob);

		return error(_("%s: Not handling case %s -> %s -> %s"),
			     path, orig_hex, our_hex, their_hex);
	}

	return 0;
}

int merge_one_file_cb(const struct object_id *orig_blob,
		      const struct object_id *our_blob,
		      const struct object_id *their_blob, const char *path,
		      unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
		      void *data)
{
	return merge_strategies_one_file((struct repository *)data,
					 orig_blob, our_blob, their_blob, path,
					 orig_mode, our_mode, their_mode);
}

int merge_program_cb(const struct object_id *orig_blob,
		     const struct object_id *our_blob,
		     const struct object_id *their_blob, const char *path,
		     unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
		     void *data)
{
	char ownbuf[3][GIT_MAX_HEXSZ] = {{0}};
	const char *arguments[] = { (char *)data, "", "", "", path,
				    ownbuf[0], ownbuf[1], ownbuf[2],
				    NULL };

	if (orig_blob)
		arguments[1] = oid_to_hex(orig_blob);
	if (our_blob)
		arguments[2] = oid_to_hex(our_blob);
	if (their_blob)
		arguments[3] = oid_to_hex(their_blob);

	xsnprintf(ownbuf[0], sizeof(ownbuf[0]), "%o", orig_mode);
	xsnprintf(ownbuf[1], sizeof(ownbuf[1]), "%o", our_mode);
	xsnprintf(ownbuf[2], sizeof(ownbuf[2]), "%o", their_mode);

	return run_command_v_opt(arguments, 0);
}

static int merge_entry(struct index_state *istate, int quiet, int pos,
		       const char *path, merge_cb cb, void *data)
{
	int found = 0;
	const struct object_id *oids[3] = {NULL};
	unsigned int modes[3] = {0};

	do {
		const struct cache_entry *ce = istate->cache[pos];
		int stage = ce_stage(ce);

		if (strcmp(ce->name, path))
			break;
		found++;
		oids[stage - 1] = &ce->oid;
		modes[stage - 1] = ce->ce_mode;
	} while (++pos < istate->cache_nr);
	if (!found)
		return error(_("%s is not in the cache"), path);

	if (cb(oids[0], oids[1], oids[2], path, modes[0], modes[1], modes[2], data)) {
		if (!quiet)
			error(_("Merge program failed"));
		return -2;
	}

	return found;
}

int merge_one_path(struct index_state *istate, int oneshot, int quiet,
		   const char *path, merge_cb cb, void *data)
{
	int pos = index_name_pos(istate, path, strlen(path)), ret;

	/*
	 * If it already exists in the cache as stage0, it's
	 * already merged and there is nothing to do.
	 */
	if (pos < 0) {
		ret = merge_entry(istate, quiet, -pos - 1, path, cb, data);
		if (ret == -1)
			return -1;
		else if (ret == -2)
			return 1;
	}
	return 0;
}

int merge_all(struct index_state *istate, int oneshot, int quiet,
	      merge_cb cb, void *data)
{
	int err = 0, i, ret;
	for (i = 0; i < istate->cache_nr; i++) {
		const struct cache_entry *ce = istate->cache[i];
		if (!ce_stage(ce))
			continue;

		ret = merge_entry(istate, quiet, i, ce->name, cb, data);
		if (ret > 0)
			i += ret - 1;
		else if (ret == -1)
			return -1;
		else if (ret == -2) {
			if (oneshot)
				err++;
			else
				return 1;
		}
	}

	return err;
}

static int add_tree(const struct object_id *oid, struct tree_desc *t)
{
	struct tree *tree;

	tree = parse_tree_indirect(oid);
	if (parse_tree(tree))
		return -1;

	init_tree_desc(t, tree->buffer, tree->size);
	return 0;
}

int merge_strategies_resolve(struct repository *r,
			     struct commit_list *bases, const char *head_arg,
			     struct commit_list *remote)
{
	int i = 0;
	struct lock_file lock = LOCK_INIT;
	struct tree_desc t[MAX_UNPACK_TREES];
	struct unpack_trees_options opts;
	struct object_id head, oid;
	struct commit_list *j;

	if (head_arg)
		get_oid(head_arg, &head);

	repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
	refresh_index(r->index, 0, NULL, NULL, NULL);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = r->index;
	opts.dst_index = r->index;
	opts.update = 1;
	opts.merge = 1;
	opts.aggressive = 1;

	for (j = bases; j && j->item; j = j->next) {
		if (add_tree(&j->item->object.oid, t + (i++)))
			goto out;
	}

	if (head_arg && add_tree(&head, t + (i++)))
		goto out;
	if (remote && add_tree(&remote->item->object.oid, t + (i++)))
		goto out;

	if (i == 1)
		opts.fn = oneway_merge;
	else if (i == 2) {
		opts.fn = twoway_merge;
		opts.initial_checkout = is_index_unborn(r->index);
	} else if (i >= 3) {
		opts.fn = threeway_merge;
		opts.head_idx = i - 1;
	}

	if (unpack_trees(i, t, &opts))
		goto out;

	puts(_("Trying simple merge."));
	write_locked_index(r->index, &lock, COMMIT_LOCK);

	if (write_index_as_tree(&oid, r->index, r->index_file,
				WRITE_TREE_SILENT, NULL)) {
		int ret;

		puts(_("Simple merge failed, trying Automatic merge."));
		repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
		ret = merge_all(r->index, 0, 0, merge_one_file_cb, r);

		write_locked_index(r->index, &lock, COMMIT_LOCK);
		return !!ret;
	}

	return 0;

 out:
	rollback_lock_file(&lock);
	return 2;
}

static int fast_forward(struct repository *r, const struct object_id *oids,
			int nr, int aggressive)
{
	int i;
	struct tree_desc t[MAX_UNPACK_TREES];
	struct unpack_trees_options opts;
	struct lock_file lock = LOCK_INIT;

	repo_read_index_preload(r, NULL, 0);
	if (refresh_index(r->index, REFRESH_QUIET, NULL, NULL, NULL))
		return -1;

	repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = r->index;
	opts.dst_index = r->index;
	opts.merge = 1;
	opts.update = 1;
	opts.aggressive = aggressive;

	for (i = 0; i < nr; i++) {
		struct tree *tree;
		tree = parse_tree_indirect(oids + i);
		if (parse_tree(tree))
			return -1;
		init_tree_desc(t + i, tree->buffer, tree->size);
	}

	if (nr == 1)
		opts.fn = oneway_merge;
	else if (nr == 2) {
		opts.fn = twoway_merge;
		opts.initial_checkout = is_index_unborn(r->index);
	} else if (nr >= 3) {
		opts.fn = threeway_merge;
		opts.head_idx = nr - 1;
	}

	if (unpack_trees(nr, t, &opts))
		return -1;

	if (write_locked_index(r->index, &lock, COMMIT_LOCK))
		return error(_("unable to write new index file"));

	return 0;
}

static int write_tree(struct repository *r, struct tree **reference_tree)
{
	struct object_id oid;
	int ret;

	ret = write_index_as_tree(&oid, r->index, r->index_file, 0, NULL);
	if (!ret)
		*reference_tree = lookup_tree(r, &oid);

	return ret;
}

int merge_strategies_octopus(struct repository *r,
			     struct commit_list *bases, const char *head_arg,
			     struct commit_list *remotes)
{
	int non_ff_merge = 0, ret = 0, references = 1;
	struct commit **reference_commit;
	struct tree *reference_tree;
	struct commit_list *j;
	struct object_id head;
	struct strbuf sb = STRBUF_INIT;

	get_oid(head_arg, &head);

	reference_commit = xcalloc(commit_list_count(remotes) + 1, sizeof(struct commit *));
	reference_commit[0] = lookup_commit_reference(r, &head);
	reference_tree = repo_get_commit_tree(r, reference_commit[0]);

	if (repo_index_has_changes(r, reference_tree, &sb)) {
		error(_("Your local changes to the following files "
			"would be overwritten by merge:\n  %s"),
		      sb.buf);
		strbuf_release(&sb);
		ret = 2;
		goto out;
	}

	for (j = remotes; j && j->item; j = j->next) {
		struct commit *c = j->item;
		struct object_id *oid = &c->object.oid;
		struct commit_list *common, *k;
		char *branch_name;
		int can_ff = 1;

		if (ret) {
			/*
			 * We allow only last one to have a
			 * hand-resolvable conflicts.  Last round failed
			 * and we still had a head to merge.
			 */
			puts(_("Automated merge did not work."));
			puts(_("Should not be doing an octopus."));

			ret = 2;
			goto out;
		}

		branch_name = merge_get_better_branch_name(oid_to_hex(oid));
		common = get_merge_bases_many(c, references, reference_commit);

		if (!common)
			die(_("Unable to find common commit with %s"), branch_name);

		for (k = common; k && !oideq(&k->item->object.oid, oid); k = k->next);

		if (k) {
			printf(_("Already up to date with %s\n"), branch_name);
			free(branch_name);
			free_commit_list(common);
			continue;
		}

		if (!non_ff_merge) {
			int i;

			for (i = 0, k = common; k && i < references && can_ff; k = k->next, i++) {
				can_ff = oideq(&k->item->object.oid,
					       &reference_commit[i]->object.oid);
			}
		}

		if (!non_ff_merge && can_ff) {
			/*
			 * The first head being merged was a
			 * fast-forward.  Advance the reference commit
			 * to the head being merged, and use that tree
			 * as the intermediate result of the merge.  We
			 * still need to count this as part of the
			 * parent set.
			 */
			struct object_id oids[2];
			printf(_("Fast-forwarding to: %s\n"), branch_name);

			oidcpy(oids, &head);
			oidcpy(oids + 1, oid);

			ret = fast_forward(r, oids, 2, 0);
			if (ret) {
				free(branch_name);
				free_commit_list(common);
				goto out;
			}

			references = 0;
			write_tree(r, &reference_tree);
		} else {
			int i = 0;
			struct tree *next = NULL;
			struct object_id oids[MAX_UNPACK_TREES];

			non_ff_merge = 1;
			printf(_("Trying simple merge with %s\n"), branch_name);

			for (k = common; k; k = k->next)
				oidcpy(oids + (i++), &k->item->object.oid);

			oidcpy(oids + (i++), &reference_tree->object.oid);
			oidcpy(oids + (i++), oid);

			if (fast_forward(r, oids, i, 1)) {
				ret = 2;

				free(branch_name);
				free_commit_list(common);

				goto out;
			}

			if (write_tree(r, &next)) {
				struct lock_file lock = LOCK_INIT;

				puts(_("Simple merge did not work, trying automatic merge."));
				repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
				ret = !!merge_all(r->index, 0, 0, merge_one_file_cb, r);
				write_locked_index(r->index, &lock, COMMIT_LOCK);

				write_tree(r, &next);
			}

			reference_tree = next;
		}

		reference_commit[references++] = c;

		free(branch_name);
		free_commit_list(common);
	}

out:
	free(reference_commit);
	return ret;
}
