#include "../cache.h"
#include "../chdir-notify.h"
#include "../config.h"
#include "../iterator.h"
#include "../lockfile.h"
#include "../refs.h"
#include "../reftable/reftable-stack.h"
#include "../reftable/reftable-record.h"
#include "../reftable/reftable-error.h"
#include "../reftable/reftable-blocksource.h"
#include "../reftable/reftable-reader.h"
#include "../reftable/reftable-iterator.h"
#include "../reftable/reftable-merged.h"
#include "../reftable/reftable-generic.h"
#include "../worktree.h"
#include "refs-internal.h"

extern struct ref_storage_be refs_be_reftable;

struct git_reftable_ref_store {
	struct ref_store base;
	unsigned int store_flags;

	int err;
	char *repo_dir;

	char *reftable_dir;
	char *worktree_reftable_dir;

	struct reftable_stack *main_stack;
	struct reftable_stack *worktree_stack;
};

static struct reftable_stack *stack_for(struct git_reftable_ref_store *store,
					const char *refname)
{
	if (store->worktree_stack == NULL)
		return store->main_stack;

	switch (ref_type(refname)) {
	case REF_TYPE_PER_WORKTREE:
	case REF_TYPE_PSEUDOREF:
	case REF_TYPE_OTHER_PSEUDOREF:
		return store->worktree_stack;
	default:
	case REF_TYPE_MAIN_PSEUDOREF:
	case REF_TYPE_NORMAL:
		return store->main_stack;
	}
}

static int git_reftable_read_raw_ref(struct ref_store *ref_store,
				     const char *refname, struct object_id *oid,
				     struct strbuf *referent,
				     unsigned int *type);

static void clear_reftable_log_record(struct reftable_log_record *log)
{
	log->old_hash = NULL;
	log->new_hash = NULL;
	log->message = NULL;
	log->refname = NULL;
	reftable_log_record_release(log);
}

static void fill_reftable_log_record(struct reftable_log_record *log)
{
	const char *info = git_committer_info(0);
	struct ident_split split = { NULL };
	int result = split_ident_line(&split, info, strlen(info));
	int sign = 1;
	assert(0 == result);

	reftable_log_record_release(log);
	log->name =
		xstrndup(split.name_begin, split.name_end - split.name_begin);
	log->email =
		xstrndup(split.mail_begin, split.mail_end - split.mail_begin);
	log->time = atol(split.date_begin);
	if (*split.tz_begin == '-') {
		sign = -1;
		split.tz_begin++;
	}
	if (*split.tz_begin == '+') {
		sign = 1;
		split.tz_begin++;
	}

	log->tz_offset = sign * atoi(split.tz_begin);
}

static int has_suffix(struct strbuf *b, const char *suffix)
{
	size_t len = strlen(suffix);

	if (len > b->len) {
		return 0;
	}

	return 0 == strncmp(b->buf + b->len - len, suffix, len);
}

/* trims the last path component of b. Returns -1 if it is not
 * present, or 0 on success
 */
static int trim_component(struct strbuf *b)
{
	char *last;
	last = strrchr(b->buf, '/');
	if (!last)
		return -1;
	strbuf_setlen(b, last - b->buf);
	return 0;
}

/* Returns whether `b` is a worktree path, trimming it to the gitdir
 */
static int is_worktree(struct strbuf *b)
{
	if (trim_component(b) < 0) {
		return 0;
	}
	if (!has_suffix(b, "/worktrees")) {
		return 0;
	}
	trim_component(b);
	return 1;
}

static struct ref_store *git_reftable_ref_store_create(const char *path,
						       unsigned int store_flags)
{
	struct git_reftable_ref_store *refs = xcalloc(1, sizeof(*refs));
	struct ref_store *ref_store = (struct ref_store *)refs;
	struct reftable_write_options cfg = {
		.block_size = 4096,
		.hash_id = the_hash_algo->format_id,
	};
	struct strbuf sb = STRBUF_INIT;
	const char *gitdir = path;
	struct strbuf wt_buf = STRBUF_INIT;
	int wt = 0;

	strbuf_addstr(&wt_buf, path);

	/* this is clumsy, but the official worktree functions (eg.
	 * get_worktrees()) function will try to initialize a ref storage
	 * backend, leading to infinite recursion.  */
	wt = is_worktree(&wt_buf);
	if (wt) {
		gitdir = wt_buf.buf;
	}

	base_ref_store_init(ref_store, &refs_be_reftable);
	ref_store->gitdir = xstrdup(gitdir);
	refs->store_flags = store_flags;
	strbuf_addf(&sb, "%s/reftable", gitdir);
	refs->reftable_dir = xstrdup(sb.buf);
	strbuf_reset(&sb);

	refs->err =
		reftable_new_stack(&refs->main_stack, refs->reftable_dir, cfg);
	assert(refs->err != REFTABLE_API_ERROR);

	if (refs->err == 0 && wt) {
		strbuf_addf(&sb, "%s/reftable", path);
		refs->worktree_reftable_dir = xstrdup(sb.buf);

		refs->err = reftable_new_stack(&refs->worktree_stack,
					       refs->worktree_reftable_dir,
					       cfg);
		assert(refs->err != REFTABLE_API_ERROR);
	}

	strbuf_release(&sb);
	strbuf_release(&wt_buf);
	return ref_store;
}

static int git_reftable_init_db(struct ref_store *ref_store, struct strbuf *err)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct strbuf sb = STRBUF_INIT;

	safe_create_dir(refs->reftable_dir, 1);
	assert(refs->worktree_reftable_dir == NULL);

	strbuf_addf(&sb, "%s/HEAD", refs->base.gitdir);
	write_file(sb.buf, "ref: refs/heads/.invalid");
	strbuf_reset(&sb);

	strbuf_addf(&sb, "%s/refs", refs->base.gitdir);
	safe_create_dir(sb.buf, 1);
	strbuf_reset(&sb);

	strbuf_addf(&sb, "%s/refs/heads", refs->base.gitdir);
	write_file(sb.buf, "this repository uses the reftable format");

	return 0;
}

struct git_reftable_iterator {
	struct ref_iterator base;
	struct reftable_iterator iter;
	struct reftable_ref_record ref;
	struct object_id oid;
	struct ref_store *ref_store;

	/* In case we must iterate over 2 stacks, this is non-null. */
	struct reftable_merged_table *merged;
	unsigned int flags;
	int err;
	const char *prefix;
};

static int reftable_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct git_reftable_iterator *ri =
		(struct git_reftable_iterator *)ref_iterator;
	while (ri->err == 0) {
		ri->err = reftable_iterator_next_ref(&ri->iter, &ri->ref);
		if (ri->err) {
			break;
		}

		if (ref_type(ri->ref.refname) == REF_TYPE_PSEUDOREF) {
			/*
			  pseudorefs, eg. HEAD, FETCH_HEAD should not be
			  produced, by default.
			 */
			continue;
		}
		ri->base.refname = ri->ref.refname;
		if (ri->prefix != NULL &&
		    strncmp(ri->prefix, ri->ref.refname, strlen(ri->prefix))) {
			ri->err = 1;
			break;
		}
		if (ri->flags & DO_FOR_EACH_PER_WORKTREE_ONLY &&
		    ref_type(ri->base.refname) != REF_TYPE_PER_WORKTREE)
			continue;

		ri->base.flags = 0;
		if (ri->ref.value != NULL) {
			hashcpy(ri->oid.hash, ri->ref.value);
		} else if (ri->ref.target != NULL) {
			int out_flags = 0;
			const char *resolved = refs_resolve_ref_unsafe(
				ri->ref_store, ri->ref.refname,
				RESOLVE_REF_READING, &ri->oid, &out_flags);
			ri->base.flags = out_flags;
			if (resolved == NULL &&
			    !(ri->flags & DO_FOR_EACH_INCLUDE_BROKEN) &&
			    (ri->base.flags & REF_ISBROKEN)) {
				continue;
			}
		}

		ri->base.oid = &ri->oid;
		if (!(ri->flags & DO_FOR_EACH_INCLUDE_BROKEN) &&
		    !ref_resolves_to_object(ri->base.refname, ri->base.oid,
					    ri->base.flags)) {
			continue;
		}

		break;
	}

	if (ri->err > 0) {
		return ITER_DONE;
	}
	if (ri->err < 0) {
		return ITER_ERROR;
	}

	return ITER_OK;
}

static int reftable_ref_iterator_peel(struct ref_iterator *ref_iterator,
				      struct object_id *peeled)
{
	struct git_reftable_iterator *ri =
		(struct git_reftable_iterator *)ref_iterator;
	if (ri->ref.target_value != NULL) {
		hashcpy(peeled->hash, ri->ref.target_value);
		return 0;
	}

	return -1;
}

static int reftable_ref_iterator_abort(struct ref_iterator *ref_iterator)
{
	struct git_reftable_iterator *ri =
		(struct git_reftable_iterator *)ref_iterator;
	reftable_ref_record_release(&ri->ref);
	reftable_iterator_destroy(&ri->iter);
	if (ri->merged) {
		reftable_merged_table_free(ri->merged);
	}
	return 0;
}

static struct ref_iterator_vtable reftable_ref_iterator_vtable = {
	reftable_ref_iterator_advance, reftable_ref_iterator_peel,
	reftable_ref_iterator_abort
};

static struct ref_iterator *
git_reftable_ref_iterator_begin(struct ref_store *ref_store, const char *prefix,
				unsigned int flags)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct git_reftable_iterator *ri = xcalloc(1, sizeof(*ri));

	if (refs->err < 0) {
		ri->err = refs->err;
	} else if (refs->worktree_stack == NULL) {
		struct reftable_merged_table *mt =
			reftable_stack_merged_table(refs->main_stack);
		ri->err = reftable_merged_table_seek_ref(mt, &ri->iter, prefix);
	} else {
		struct reftable_merged_table *mt1 =
			reftable_stack_merged_table(refs->main_stack);
		struct reftable_merged_table *mt2 =
			reftable_stack_merged_table(refs->worktree_stack);
		struct reftable_table *tabs =
			xcalloc(2, sizeof(struct reftable_table));
		reftable_table_from_merged_table(&tabs[0], mt1);
		reftable_table_from_merged_table(&tabs[1], mt2);
		ri->err = reftable_new_merged_table(&ri->merged, tabs, 2,
						    the_hash_algo->format_id);
		if (ri->err == 0)
			ri->err = reftable_merged_table_seek_ref(
				ri->merged, &ri->iter, prefix);
	}

	base_ref_iterator_init(&ri->base, &reftable_ref_iterator_vtable, 1);
	ri->prefix = prefix;
	ri->base.oid = &ri->oid;
	ri->flags = flags;
	ri->ref_store = ref_store;
	return &ri->base;
}

static int fixup_symrefs(struct ref_store *ref_store,
			 struct ref_transaction *transaction)
{
	struct strbuf referent = STRBUF_INIT;
	int i = 0;
	int err = 0;

	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *update = transaction->updates[i];
		struct object_id old_oid;

		err = git_reftable_read_raw_ref(ref_store, update->refname,
						&old_oid, &referent,
						/* mutate input, like
						   files-backend.c */
						&update->type);
		if (err < 0 && errno == ENOENT &&
		    is_null_oid(&update->old_oid)) {
			err = 0;
		}
		if (err < 0)
			goto done;

		if (!(update->type & REF_ISSYMREF))
			continue;

		if (update->flags & REF_NO_DEREF) {
			/* what should happen here? See files-backend.c
			 * lock_ref_for_update. */
		} else {
			/*
			  If we are updating a symref (eg. HEAD), we should also
			  update the branch that the symref points to.

			  This is generic functionality, and would be better
			  done in refs.c, but the current implementation is
			  intertwined with the locking in files-backend.c.
			*/
			int new_flags = update->flags;
			struct ref_update *new_update = NULL;

			/* if this is an update for HEAD, should also record a
			   log entry for HEAD? See files-backend.c,
			   split_head_update()
			*/
			new_update = ref_transaction_add_update(
				transaction, referent.buf, new_flags,
				&update->new_oid, &update->old_oid,
				update->msg);
			new_update->parent_update = update;

			/* files-backend sets REF_LOG_ONLY here. */
			update->flags |= REF_NO_DEREF | REF_LOG_ONLY;
			update->flags &= ~REF_HAVE_OLD;
		}
	}

done:
	assert(err != REFTABLE_API_ERROR);
	strbuf_release(&referent);
	return err;
}

static int git_reftable_transaction_prepare(struct ref_store *ref_store,
					    struct ref_transaction *transaction,
					    struct strbuf *errbuf)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_addition *add = NULL;
	struct reftable_stack *stack =
		transaction->nr ?
			      stack_for(refs, transaction->updates[0]->refname) :
			      refs->main_stack;
	int err = refs->err;
	if (err < 0) {
		goto done;
	}

	err = reftable_stack_reload(stack);
	if (err) {
		goto done;
	}

	err = reftable_stack_new_addition(&add, stack);
	if (err) {
		goto done;
	}

	err = fixup_symrefs(ref_store, transaction);
	if (err) {
		goto done;
	}

	transaction->backend_data = add;
	transaction->state = REF_TRANSACTION_PREPARED;

done:
	assert(err != REFTABLE_API_ERROR);
	if (err < 0) {
		transaction->state = REF_TRANSACTION_CLOSED;
		strbuf_addf(errbuf, "reftable: transaction prepare: %s",
			    reftable_error_str(err));
	}

	return err;
}

static int git_reftable_transaction_abort(struct ref_store *ref_store,
					  struct ref_transaction *transaction,
					  struct strbuf *err)
{
	struct reftable_addition *add =
		(struct reftable_addition *)transaction->backend_data;
	reftable_addition_destroy(add);
	transaction->backend_data = NULL;
	return 0;
}

static int reftable_check_old_oid(struct ref_store *refs, const char *refname,
				  struct object_id *want_oid)
{
	struct object_id out_oid;
	int out_flags = 0;
	const char *resolved = refs_resolve_ref_unsafe(
		refs, refname, RESOLVE_REF_READING, &out_oid, &out_flags);
	if (is_null_oid(want_oid) != (resolved == NULL)) {
		return REFTABLE_LOCK_ERROR;
	}

	if (resolved != NULL && !oideq(&out_oid, want_oid)) {
		return REFTABLE_LOCK_ERROR;
	}

	return 0;
}

static int ref_update_cmp(const void *a, const void *b)
{
	return strcmp((*(struct ref_update **)a)->refname,
		      (*(struct ref_update **)b)->refname);
}

static int write_transaction_table(struct reftable_writer *writer, void *arg)
{
	struct ref_transaction *transaction = (struct ref_transaction *)arg;
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)transaction->ref_store;
	struct reftable_stack *stack =
		stack_for(refs, transaction->updates[0]->refname);
	uint64_t ts = reftable_stack_next_update_index(stack);
	int err = 0;
	int i = 0;
	struct reftable_log_record *logs =
		calloc(transaction->nr, sizeof(*logs));
	struct ref_update **sorted =
		malloc(transaction->nr * sizeof(struct ref_update *));
	COPY_ARRAY(sorted, transaction->updates, transaction->nr);
	QSORT(sorted, transaction->nr, ref_update_cmp);
	reftable_writer_set_limits(writer, ts, ts);

	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *u = sorted[i];
		struct reftable_log_record *log = &logs[i];
		fill_reftable_log_record(log);
		log->refname = (char *)u->refname;
		log->old_hash = u->old_oid.hash;
		log->new_hash = u->new_oid.hash;
		log->update_index = ts;
		log->message = u->msg;

		if (u->flags & REF_LOG_ONLY) {
			continue;
		}

		if (u->flags & REF_HAVE_NEW) {
			struct reftable_ref_record ref = { NULL };
			struct object_id peeled;

			int peel_error = peel_object(&u->new_oid, &peeled);
			ref.refname = (char *)u->refname;

			if (!is_null_oid(&u->new_oid)) {
				ref.value = u->new_oid.hash;
			}
			ref.update_index = ts;
			if (!peel_error) {
				ref.target_value = peeled.hash;
			}

			err = reftable_writer_add_ref(writer, &ref);
			if (err < 0) {
				goto done;
			}
		}
	}

	for (i = 0; i < transaction->nr; i++) {
		err = reftable_writer_add_log(writer, &logs[i]);
		clear_reftable_log_record(&logs[i]);
		if (err < 0) {
			goto done;
		}
	}

done:
	assert(err != REFTABLE_API_ERROR);
	free(logs);
	free(sorted);
	return err;
}

static int git_reftable_transaction_finish(struct ref_store *ref_store,
					   struct ref_transaction *transaction,
					   struct strbuf *errmsg)
{
	struct reftable_addition *add =
		(struct reftable_addition *)transaction->backend_data;
	int err = 0;
	int i;

	for (i = 0; i < transaction->nr; i++) {
		struct ref_update *u = transaction->updates[i];
		if (u->flags & REF_HAVE_OLD) {
			err = reftable_check_old_oid(transaction->ref_store,
						     u->refname, &u->old_oid);
			if (err < 0) {
				goto done;
			}
		}
	}
	if (transaction->nr) {
		err = reftable_addition_add(add, &write_transaction_table,
					    transaction);
		if (err < 0) {
			goto done;
		}
	}

	err = reftable_addition_commit(add);

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_addition_destroy(add);
	transaction->state = REF_TRANSACTION_CLOSED;
	transaction->backend_data = NULL;
	if (err) {
		strbuf_addf(errmsg, "reftable: transaction failure: %s",
			    reftable_error_str(err));
		return -1;
	}
	return err;
}

static int
git_reftable_transaction_initial_commit(struct ref_store *ref_store,
					struct ref_transaction *transaction,
					struct strbuf *errmsg)
{
	int err = git_reftable_transaction_prepare(ref_store, transaction,
						   errmsg);
	if (err)
		return err;

	return git_reftable_transaction_finish(ref_store, transaction, errmsg);
}

struct write_delete_refs_arg {
	struct reftable_stack *stack;
	struct string_list *refnames;
	const char *logmsg;
	unsigned int flags;
};

static int write_delete_refs_table(struct reftable_writer *writer, void *argv)
{
	struct write_delete_refs_arg *arg =
		(struct write_delete_refs_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->stack);
	int err = 0;
	int i = 0;

	reftable_writer_set_limits(writer, ts, ts);
	for (i = 0; i < arg->refnames->nr; i++) {
		struct reftable_ref_record ref = {
			.refname = (char *)arg->refnames->items[i].string,
			.update_index = ts,
		};
		err = reftable_writer_add_ref(writer, &ref);
		if (err < 0) {
			return err;
		}
	}

	for (i = 0; i < arg->refnames->nr; i++) {
		struct reftable_log_record log = {
			.update_index = ts,
		};
		struct reftable_ref_record current = { NULL };
		fill_reftable_log_record(&log);
		log.message = xstrdup(arg->logmsg);
		log.new_hash = NULL;
		log.old_hash = NULL;
		log.update_index = ts;
		log.refname = (char *)arg->refnames->items[i].string;

		if (reftable_stack_read_ref(arg->stack, log.refname,
					    &current) == 0) {
			log.old_hash = current.value;
		}
		err = reftable_writer_add_log(writer, &log);
		log.old_hash = NULL;
		reftable_ref_record_release(&current);

		clear_reftable_log_record(&log);
		if (err < 0) {
			return err;
		}
	}
	return 0;
}

static int git_reftable_delete_refs(struct ref_store *ref_store,
				    const char *msg,
				    struct string_list *refnames,
				    unsigned int flags)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack =
		stack_for(refs, refnames->items[0].string);
	struct write_delete_refs_arg arg = {
		.stack = stack,
		.refnames = refnames,
		.logmsg = msg,
		.flags = flags,
	};
	int err = refs->err;
	if (err < 0) {
		goto done;
	}

	string_list_sort(refnames);
	err = reftable_stack_reload(stack);
	if (err) {
		goto done;
	}
	err = reftable_stack_add(stack, &write_delete_refs_table, &arg);
done:
	assert(err != REFTABLE_API_ERROR);
	return err;
}

static int git_reftable_pack_refs(struct ref_store *ref_store,
				  unsigned int flags)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	int err = refs->err;
	if (err < 0) {
		return err;
	}
	err = reftable_stack_compact_all(refs->main_stack, NULL);
	if (err == 0 && refs->worktree_stack != NULL)
		err = reftable_stack_compact_all(refs->worktree_stack, NULL);
	return err;
}

struct write_create_symref_arg {
	struct git_reftable_ref_store *refs;
	struct reftable_stack *stack;
	const char *refname;
	const char *target;
	const char *logmsg;
};

static int write_create_symref_table(struct reftable_writer *writer, void *arg)
{
	struct write_create_symref_arg *create =
		(struct write_create_symref_arg *)arg;
	uint64_t ts = reftable_stack_next_update_index(create->stack);
	int err = 0;

	struct reftable_ref_record ref = {
		.refname = (char *)create->refname,
		.target = (char *)create->target,
		.update_index = ts,
	};
	reftable_writer_set_limits(writer, ts, ts);
	err = reftable_writer_add_ref(writer, &ref);
	if (err == 0) {
		struct reftable_log_record log = { NULL };
		struct object_id new_oid;
		struct object_id old_oid;

		fill_reftable_log_record(&log);
		log.refname = (char *)create->refname;
		log.message = (char *)create->logmsg;
		log.update_index = ts;
		if (refs_resolve_ref_unsafe(
			    (struct ref_store *)create->refs, create->refname,
			    RESOLVE_REF_READING, &old_oid, NULL) != NULL) {
			log.old_hash = old_oid.hash;
		}

		if (refs_resolve_ref_unsafe((struct ref_store *)create->refs,
					    create->target, RESOLVE_REF_READING,
					    &new_oid, NULL) != NULL) {
			log.new_hash = new_oid.hash;
		}

		if (log.old_hash != NULL || log.new_hash != NULL) {
			err = reftable_writer_add_log(writer, &log);
		}
		log.refname = NULL;
		log.message = NULL;
		log.old_hash = NULL;
		log.new_hash = NULL;
		clear_reftable_log_record(&log);
	}
	return err;
}

static int git_reftable_create_symref(struct ref_store *ref_store,
				      const char *refname, const char *target,
				      const char *logmsg)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, refname);
	struct write_create_symref_arg arg = { .refs = refs,
					       .stack = stack,
					       .refname = refname,
					       .target = target,
					       .logmsg = logmsg };
	int err = refs->err;
	if (err < 0) {
		goto done;
	}
	err = reftable_stack_reload(stack);
	if (err) {
		goto done;
	}
	err = reftable_stack_add(stack, &write_create_symref_table, &arg);
done:
	assert(err != REFTABLE_API_ERROR);
	return err;
}

struct write_rename_arg {
	struct reftable_stack *stack;
	const char *oldname;
	const char *newname;
	const char *logmsg;
};

static int write_rename_table(struct reftable_writer *writer, void *argv)
{
	struct write_rename_arg *arg = (struct write_rename_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->stack);
	struct reftable_ref_record ref = { NULL };
	int err = reftable_stack_read_ref(arg->stack, arg->oldname, &ref);

	if (err) {
		goto done;
	}

	/* XXX do ref renames overwrite the target? */
	if (reftable_stack_read_ref(arg->stack, arg->newname, &ref) == 0) {
		goto done;
	}

	free(ref.refname);
	ref.refname = strdup(arg->newname);
	reftable_writer_set_limits(writer, ts, ts);
	ref.update_index = ts;

	{
		struct reftable_ref_record todo[2] = { { NULL } };
		todo[0].refname = (char *)arg->oldname;
		todo[0].update_index = ts;
		/* leave todo[0] empty */
		todo[1] = ref;
		todo[1].update_index = ts;

		err = reftable_writer_add_refs(writer, todo, 2);
		if (err < 0) {
			goto done;
		}
	}

	if (ref.value != NULL) {
		struct reftable_log_record todo[2] = { { NULL } };
		fill_reftable_log_record(&todo[0]);
		fill_reftable_log_record(&todo[1]);

		todo[0].refname = (char *)arg->oldname;
		todo[0].update_index = ts;
		todo[0].message = (char *)arg->logmsg;
		todo[0].old_hash = ref.value;
		todo[0].new_hash = NULL;

		todo[1].refname = (char *)arg->newname;
		todo[1].update_index = ts;
		todo[1].old_hash = NULL;
		todo[1].new_hash = ref.value;
		todo[1].message = (char *)arg->logmsg;

		err = reftable_writer_add_logs(writer, todo, 2);

		clear_reftable_log_record(&todo[0]);
		clear_reftable_log_record(&todo[1]);

		if (err < 0) {
			goto done;
		}

	} else {
		/* XXX symrefs? */
	}

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_ref_record_release(&ref);
	return err;
}

static int git_reftable_rename_ref(struct ref_store *ref_store,
				   const char *oldrefname,
				   const char *newrefname, const char *logmsg)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, newrefname);
	struct write_rename_arg arg = {
		.stack = stack,
		.oldname = oldrefname,
		.newname = newrefname,
		.logmsg = logmsg,
	};
	int err = refs->err;
	if (err < 0) {
		goto done;
	}
	err = reftable_stack_reload(stack);
	if (err) {
		goto done;
	}

	err = reftable_stack_add(stack, &write_rename_table, &arg);
done:
	assert(err != REFTABLE_API_ERROR);
	return err;
}

static int git_reftable_copy_ref(struct ref_store *ref_store,
				 const char *oldrefname, const char *newrefname,
				 const char *logmsg)
{
	BUG("reftable reference store does not support copying references");
}

struct git_reftable_reflog_ref_iterator {
	struct ref_iterator base;
	struct reftable_iterator iter;
	struct reftable_log_record log;
	struct object_id oid;

	/* Used when iterating over worktree & main */
	struct reftable_merged_table *merged;
	char *last_name;
};

static int
git_reftable_reflog_ref_iterator_advance(struct ref_iterator *ref_iterator)
{
	struct git_reftable_reflog_ref_iterator *ri =
		(struct git_reftable_reflog_ref_iterator *)ref_iterator;

	while (1) {
		int err = reftable_iterator_next_log(&ri->iter, &ri->log);
		if (err > 0) {
			return ITER_DONE;
		}
		if (err < 0) {
			return ITER_ERROR;
		}

		ri->base.refname = ri->log.refname;
		if (ri->last_name != NULL &&
		    !strcmp(ri->log.refname, ri->last_name)) {
			/* we want the refnames that we have reflogs for, so we
			 * skip if we've already produced this name. This could
			 * be faster by seeking directly to
			 * reflog@update_index==0.
			 */
			continue;
		}

		free(ri->last_name);
		ri->last_name = xstrdup(ri->log.refname);
		hashcpy(ri->oid.hash, ri->log.new_hash);
		return ITER_OK;
	}
}

static int
git_reftable_reflog_ref_iterator_peel(struct ref_iterator *ref_iterator,
				      struct object_id *peeled)
{
	BUG("not supported.");
	return -1;
}

static int
git_reftable_reflog_ref_iterator_abort(struct ref_iterator *ref_iterator)
{
	struct git_reftable_reflog_ref_iterator *ri =
		(struct git_reftable_reflog_ref_iterator *)ref_iterator;
	reftable_log_record_release(&ri->log);
	reftable_iterator_destroy(&ri->iter);
	if (ri->merged)
		reftable_merged_table_free(ri->merged);
	return 0;
}

static struct ref_iterator_vtable git_reftable_reflog_ref_iterator_vtable = {
	git_reftable_reflog_ref_iterator_advance,
	git_reftable_reflog_ref_iterator_peel,
	git_reftable_reflog_ref_iterator_abort
};

static struct ref_iterator *
git_reftable_reflog_iterator_begin(struct ref_store *ref_store)
{
	struct git_reftable_reflog_ref_iterator *ri = xcalloc(sizeof(*ri), 1);
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;

	if (refs->worktree_stack == NULL) {
		struct reftable_stack *stack = refs->main_stack;
		struct reftable_merged_table *mt =
			reftable_stack_merged_table(stack);
		int err = reftable_merged_table_seek_log(mt, &ri->iter, "");
		if (err < 0) {
			free(ri);
			/* XXX is this allowed? */
			return NULL;
		}
	} else {
		struct reftable_merged_table *mt1 =
			reftable_stack_merged_table(refs->main_stack);
		struct reftable_merged_table *mt2 =
			reftable_stack_merged_table(refs->worktree_stack);
		struct reftable_table *tabs =
			xcalloc(2, sizeof(struct reftable_table));
		int err = 0;
		reftable_table_from_merged_table(&tabs[0], mt1);
		reftable_table_from_merged_table(&tabs[1], mt2);
		err = reftable_new_merged_table(&ri->merged, tabs, 2,
						the_hash_algo->format_id);
		if (err < 0) {
			free(tabs);
			/* XXX see above */
			return NULL;
		}
		err = reftable_merged_table_seek_ref(ri->merged, &ri->iter, "");
		if (err < 0) {
			return NULL;
		}
	}
	base_ref_iterator_init(&ri->base,
			       &git_reftable_reflog_ref_iterator_vtable, 1);
	ri->base.oid = &ri->oid;

	return (struct ref_iterator *)ri;
}

static int git_reftable_for_each_reflog_ent_newest_first(
	struct ref_store *ref_store, const char *refname, each_reflog_ent_fn fn,
	void *cb_data)
{
	struct reftable_iterator it = { NULL };
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, refname);
	struct reftable_merged_table *mt = NULL;
	int err = 0;
	struct reftable_log_record log = { NULL };

	if (refs->err < 0) {
		return refs->err;
	}

	mt = reftable_stack_merged_table(stack);
	err = reftable_merged_table_seek_log(mt, &it, refname);
	while (err == 0) {
		struct object_id old_oid;
		struct object_id new_oid;
		const char *full_committer = "";

		err = reftable_iterator_next_log(&it, &log);
		if (err > 0) {
			err = 0;
			break;
		}
		if (err < 0) {
			break;
		}

		if (strcmp(log.refname, refname)) {
			break;
		}

		hashcpy(old_oid.hash, log.old_hash);
		hashcpy(new_oid.hash, log.new_hash);

		full_committer = fmt_ident(log.name, log.email,
					   WANT_COMMITTER_IDENT,
					   /*date*/ NULL, IDENT_NO_DATE);
		err = fn(&old_oid, &new_oid, full_committer, log.time,
			 log.tz_offset, log.message, cb_data);
		if (err)
			break;
	}

	reftable_log_record_release(&log);
	reftable_iterator_destroy(&it);
	return err;
}

static int git_reftable_for_each_reflog_ent_oldest_first(
	struct ref_store *ref_store, const char *refname, each_reflog_ent_fn fn,
	void *cb_data)
{
	struct reftable_iterator it = { NULL };
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, refname);
	struct reftable_merged_table *mt = NULL;
	struct reftable_log_record *logs = NULL;
	int cap = 0;
	int len = 0;
	int err = 0;
	int i = 0;

	if (refs->err < 0) {
		return refs->err;
	}
	mt = reftable_stack_merged_table(stack);
	err = reftable_merged_table_seek_log(mt, &it, refname);

	while (err == 0) {
		struct reftable_log_record log = { NULL };
		err = reftable_iterator_next_log(&it, &log);
		if (err > 0) {
			err = 0;
			break;
		}
		if (err < 0) {
			break;
		}

		if (strcmp(log.refname, refname)) {
			break;
		}

		if (len == cap) {
			cap = 2 * cap + 1;
			logs = realloc(logs, cap * sizeof(*logs));
		}

		logs[len++] = log;
	}

	for (i = len; i--;) {
		struct reftable_log_record *log = &logs[i];
		struct object_id old_oid;
		struct object_id new_oid;
		const char *full_committer = "";

		hashcpy(old_oid.hash, log->old_hash);
		hashcpy(new_oid.hash, log->new_hash);

		full_committer = fmt_ident(log->name, log->email,
					   WANT_COMMITTER_IDENT, NULL,
					   IDENT_NO_DATE);
		err = fn(&old_oid, &new_oid, full_committer, log->time,
			 log->tz_offset, log->message, cb_data);
		if (err) {
			break;
		}
	}

	for (i = 0; i < len; i++) {
		reftable_log_record_release(&logs[i]);
	}
	free(logs);

	reftable_iterator_destroy(&it);
	return err;
}

static int git_reftable_reflog_exists(struct ref_store *ref_store,
				      const char *refname)
{
	struct reftable_iterator it = { NULL };
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, refname);
	struct reftable_merged_table *mt = reftable_stack_merged_table(stack);
	struct reftable_log_record log = { NULL };
	int err = refs->err;

	if (err < 0) {
		goto done;
	}
	err = reftable_merged_table_seek_log(mt, &it, refname);
	if (err) {
		goto done;
	}
	err = reftable_iterator_next_log(&it, &log);
	if (err) {
		goto done;
	}

	if (strcmp(log.refname, refname)) {
		err = 1;
	}

done:
	reftable_iterator_destroy(&it);
	reftable_log_record_release(&log);
	return !err;
}

static int git_reftable_create_reflog(struct ref_store *ref_store,
				      const char *refname, int force_create,
				      struct strbuf *err)
{
	return 0;
}

static int git_reftable_delete_reflog(struct ref_store *ref_store,
				      const char *refname)
{
	return 0;
}

struct reflog_expiry_arg {
	struct git_reftable_ref_store *refs;
	struct reftable_stack *stack;
	struct reftable_log_record *tombstones;
	int len;
	int cap;
};

static void clear_log_tombstones(struct reflog_expiry_arg *arg)
{
	int i = 0;
	for (; i < arg->len; i++) {
		reftable_log_record_release(&arg->tombstones[i]);
	}

	FREE_AND_NULL(arg->tombstones);
}

static void add_log_tombstone(struct reflog_expiry_arg *arg,
			      const char *refname, uint64_t ts)
{
	struct reftable_log_record tombstone = {
		.refname = xstrdup(refname),
		.update_index = ts,
	};
	if (arg->len == arg->cap) {
		arg->cap = 2 * arg->cap + 1;
		arg->tombstones =
			realloc(arg->tombstones, arg->cap * sizeof(tombstone));
	}
	arg->tombstones[arg->len++] = tombstone;
}

static int write_reflog_expiry_table(struct reftable_writer *writer, void *argv)
{
	struct reflog_expiry_arg *arg = (struct reflog_expiry_arg *)argv;
	uint64_t ts = reftable_stack_next_update_index(arg->stack);
	int i = 0;
	reftable_writer_set_limits(writer, ts, ts);
	for (i = 0; i < arg->len; i++) {
		int err = reftable_writer_add_log(writer, &arg->tombstones[i]);
		if (err) {
			return err;
		}
	}
	return 0;
}

static int
git_reftable_reflog_expire(struct ref_store *ref_store, const char *refname,
			   const struct object_id *oid, unsigned int flags,
			   reflog_expiry_prepare_fn prepare_fn,
			   reflog_expiry_should_prune_fn should_prune_fn,
			   reflog_expiry_cleanup_fn cleanup_fn,
			   void *policy_cb_data)
{
	/*
	  For log expiry, we write tombstones in place of the expired entries,
	  This means that the entries are still retrievable by delving into the
	  stack, and expiring entries paradoxically takes extra memory.

	  This memory is only reclaimed when some operation issues a
	  git_reftable_pack_refs(), which will compact the entire stack and get
	  rid of deletion entries.

	  It would be better if the refs backend supported an API that sets a
	  criterion for all refs, passing the criterion to pack_refs().
	*/
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, refname);
	struct reftable_merged_table *mt = NULL;
	struct reflog_expiry_arg arg = {
		.stack = stack,
		.refs = refs,
	};
	struct reftable_log_record log = { NULL };
	struct reftable_iterator it = { NULL };
	int err = 0;
	if (refs->err < 0) {
		return refs->err;
	}
	err = reftable_stack_reload(stack);
	if (err) {
		goto done;
	}

	mt = reftable_stack_merged_table(stack);
	err = reftable_merged_table_seek_log(mt, &it, refname);
	if (err < 0) {
		goto done;
	}

	while (1) {
		struct object_id ooid;
		struct object_id noid;

		int err = reftable_iterator_next_log(&it, &log);
		if (err < 0) {
			goto done;
		}

		if (err > 0 || strcmp(log.refname, refname)) {
			break;
		}
		hashcpy(ooid.hash, log.old_hash);
		hashcpy(noid.hash, log.new_hash);

		if (should_prune_fn(&ooid, &noid, log.email,
				    (timestamp_t)log.time, log.tz_offset,
				    log.message, policy_cb_data)) {
			add_log_tombstone(&arg, refname, log.update_index);
		}
	}
	err = reftable_stack_add(stack, &write_reflog_expiry_table, &arg);

done:
	assert(err != REFTABLE_API_ERROR);
	reftable_log_record_release(&log);
	reftable_iterator_destroy(&it);
	clear_log_tombstones(&arg);
	return err;
}

static int reftable_error_to_errno(int err)
{
	switch (err) {
	case REFTABLE_IO_ERROR:
		return EIO;
	case REFTABLE_FORMAT_ERROR:
		return EFAULT;
	case REFTABLE_NOT_EXIST_ERROR:
		return ENOENT;
	case REFTABLE_LOCK_ERROR:
		return EBUSY;
	case REFTABLE_API_ERROR:
		return EINVAL;
	case REFTABLE_ZLIB_ERROR:
		return EDOM;
	default:
		return ERANGE;
	}
}

static int git_reftable_read_raw_ref(struct ref_store *ref_store,
				     const char *refname, struct object_id *oid,
				     struct strbuf *referent,
				     unsigned int *type)
{
	struct git_reftable_ref_store *refs =
		(struct git_reftable_ref_store *)ref_store;
	struct reftable_stack *stack = stack_for(refs, refname);

	struct reftable_ref_record ref = { NULL };
	int err = 0;
	if (refs->err < 0) {
		return refs->err;
	}

	/* This is usually not needed, but Git doesn't signal to ref backend if
	   a subprocess updated the ref DB.  So we always check.
	*/
	err = reftable_stack_reload(stack);
	if (err) {
		goto done;
	}

	err = reftable_stack_read_ref(stack, refname, &ref);
	if (err > 0) {
		errno = ENOENT;
		err = -1;
		goto done;
	}
	if (err < 0) {
		errno = reftable_error_to_errno(err);
		err = -1;
		goto done;
	}
	if (ref.target != NULL) {
		strbuf_reset(referent);
		strbuf_addstr(referent, ref.target);
		*type |= REF_ISSYMREF;
	} else if (ref.value != NULL) {
		hashcpy(oid->hash, ref.value);
	} else {
		*type |= REF_ISBROKEN;
		errno = EINVAL;
		err = -1;
	}
done:
	assert(err != REFTABLE_API_ERROR);
	reftable_ref_record_release(&ref);
	return err;
}

struct ref_storage_be refs_be_reftable = {
	&refs_be_files,
	"reftable",
	git_reftable_ref_store_create,
	git_reftable_init_db,
	git_reftable_transaction_prepare,
	git_reftable_transaction_finish,
	git_reftable_transaction_abort,
	git_reftable_transaction_initial_commit,

	git_reftable_pack_refs,
	git_reftable_create_symref,
	git_reftable_delete_refs,
	git_reftable_rename_ref,
	git_reftable_copy_ref,

	git_reftable_ref_iterator_begin,
	git_reftable_read_raw_ref,

	git_reftable_reflog_iterator_begin,
	git_reftable_for_each_reflog_ent_oldest_first,
	git_reftable_for_each_reflog_ent_newest_first,
	git_reftable_reflog_exists,
	git_reftable_create_reflog,
	git_reftable_delete_reflog,
	git_reftable_reflog_expire,
};
