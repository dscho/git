/*
 * "Ostensibly Recursive's Twin" merge strategy, or "ort" for short.  Meant
 * as a drop-in replacement for the "recursive" merge strategy, allowing one
 * to replace
 *
 *   git merge [-s recursive]
 *
 * with
 *
 *   git merge -s ort
 *
 * Note: git's parser allows the space between '-s' and its argument to be
 * missing.  (Should I have backronymed "ham", "alsa", "kip", "nap, "alvo",
 * "cale", "peedy", or "ins" instead of "ort"?)
 */

#include "cache.h"
#include "merge-ort.h"

#include "cache-tree.h"
#include "diff.h"
#include "diffcore.h"
#include "dir.h"
#include "object-store.h"
#include "strmap.h"
#include "tree.h"
#include "unpack-trees.h"
#include "xdiff-interface.h"

struct merge_options_internal {
	struct strmap paths;    /* maps path -> (merged|conflict)_info */
	struct strmap unmerged; /* maps path -> conflict_info */
	const char *current_dir_name;
	int call_depth;
};

struct version_info {
	struct object_id oid;
	unsigned short mode;
};

struct merged_info {
	struct version_info result;
	unsigned is_null:1;
	unsigned clean:1;
	size_t basename_offset;
	 /*
	  * Containing directory name.  Note that we assume directory_name is
	  * constructed such that
	  *    strcmp(dir1_name, dir2_name) == 0 iff dir1_name == dir2_name,
	  * i.e. string equality is equivalent to pointer equality.  For this
	  * to hold, we have to be careful setting directory_name.
	  */
	const char *directory_name;
};

struct conflict_info {
	struct merged_info merged;
	struct version_info stages[3];
	const char *pathnames[3];
	unsigned df_conflict:1;
	unsigned path_conflict:1;
	unsigned filemask:3;
	unsigned dirmask:3;
	unsigned match_mask:3;
};

static int err(struct merge_options *opt, const char *err, ...)
{
	va_list params;
	struct strbuf sb = STRBUF_INIT;

	strbuf_addstr(&sb, "error: ");
	va_start(params, err);
	strbuf_vaddf(&sb, err, params);
	va_end(params);

	error("%s", sb.buf);
	strbuf_release(&sb);

	return -1;
}

static void free_strmap_strings(struct strmap *map)
{
	struct hashmap_iter iter;
	struct strmap_entry *entry;

	strmap_for_each_entry(map, &iter, entry) {
		free((char*)entry->key);
	}
}

static void setup_path_info(struct merge_options *opt,
			    struct string_list_item *result,
			    const char *current_dir_name,
			    int current_dir_name_len,
			    char *fullpath, /* we'll take over ownership */
			    struct name_entry *names,
			    struct name_entry *merged_version,
			    unsigned is_null,     /* boolean */
			    unsigned df_conflict, /* boolean */
			    unsigned filemask,
			    unsigned dirmask,
			    int resolved          /* boolean */)
{
	struct conflict_info *path_info;

	assert(!is_null || resolved);
	assert(!df_conflict || !resolved); /* df_conflict implies !resolved */
	assert(resolved == (merged_version != NULL));

	path_info = xcalloc(1, resolved ? sizeof(struct merged_info) :
					  sizeof(struct conflict_info));
	path_info->merged.directory_name = current_dir_name;
	path_info->merged.basename_offset = current_dir_name_len;
	path_info->merged.clean = !!resolved;
	if (resolved) {
		path_info->merged.result.mode = merged_version->mode;
		oidcpy(&path_info->merged.result.oid, &merged_version->oid);
		path_info->merged.is_null = !!is_null;
	} else {
		int i;

		for (i = 0; i < 3; i++) {
			path_info->pathnames[i] = fullpath;
			path_info->stages[i].mode = names[i].mode;
			oidcpy(&path_info->stages[i].oid, &names[i].oid);
		}
		path_info->filemask = filemask;
		path_info->dirmask = dirmask;
		path_info->df_conflict = !!df_conflict;
	}
	strmap_put(&opt->priv->paths, fullpath, path_info);
	result->string = fullpath;
	result->util = path_info;
}

static int collect_merge_info_callback(int n,
				       unsigned long mask,
				       unsigned long dirmask,
				       struct name_entry *names,
				       struct traverse_info *info)
{
	/*
	 * n is 3.  Always.
	 * common ancestor (mbase) has mask 1, and stored in index 0 of names
	 * head of side 1  (side1) has mask 2, and stored in index 1 of names
	 * head of side 2  (side2) has mask 4, and stored in index 2 of names
	 */
	struct merge_options *opt = info->data;
	struct merge_options_internal *opti = opt->priv;
	struct string_list_item pi;  /* Path Info */
	struct conflict_info *ci; /* pi.util when there's a conflict */
	struct name_entry *p;
	size_t len;
	char *fullpath;
	const char *dirname = opti->current_dir_name;
	unsigned filemask = mask & ~dirmask;
	unsigned match_mask = 0; /* will be updated below */
	unsigned mbase_null = !(mask & 1);
	unsigned side1_null = !(mask & 2);
	unsigned side2_null = !(mask & 4);
	unsigned side1_matches_mbase = (!side1_null && !mbase_null &&
					names[0].mode == names[1].mode &&
					oideq(&names[0].oid, &names[1].oid));
	unsigned side2_matches_mbase = (!side2_null && !mbase_null &&
					names[0].mode == names[2].mode &&
					oideq(&names[0].oid, &names[2].oid));
	unsigned sides_match = (!side1_null && !side2_null &&
				names[1].mode == names[2].mode &&
				oideq(&names[1].oid, &names[2].oid));
	/*
	 * Note: We only label files with df_conflict, not directories.
	 * Since directories stay where they are, and files move out of the
	 * way to make room for a directory, we don't care if there was a
	 * directory/file conflict for a parent directory of the current path.
	 */
	unsigned df_conflict = (filemask != 0) && (dirmask != 0);

	/* n = 3 is a fundamental assumption. */
	if (n != 3)
		BUG("Called collect_merge_info_callback wrong");

	/*
	 * A bunch of sanity checks verifying that traverse_trees() calls
	 * us the way I expect.  Could just remove these at some point,
	 * though maybe they are helpful to future code readers.
	 */
	assert(mbase_null == is_null_oid(&names[0].oid));
	assert(side1_null == is_null_oid(&names[1].oid));
	assert(side2_null == is_null_oid(&names[2].oid));
	assert(!mbase_null || !side1_null || !side2_null);
	assert(mask > 0 && mask < 8);

	/* Other invariant checks, mostly for documentation purposes. */
	assert(mask == (dirmask | filemask));

	/* Determine match_mask */
	if (side1_matches_mbase)
		match_mask = (side2_matches_mbase ? 7 : 3);
	else if (side2_matches_mbase)
		match_mask = 5;
	else if (sides_match)
		match_mask = 6;

	/*
	 * Get the name of the relevant filepath, which we'll pass to
	 * setup_path_info() for tracking.
	 */
	p = names;
	while (!p->mode)
		p++;
	len = traverse_path_len(info, p->pathlen);

	/* +1 in both of the following lines to include the NUL byte */
	fullpath = xmalloc(len+1);
	make_traverse_path(fullpath, len+1, info, p->path, p->pathlen);

	/*
	 * If mbase, side1, and side2 all match, we can resolve early.  Even
	 * if these are trees, there will be no renames or anything
	 * underneath.
	 */
	if (side1_matches_mbase && side2_matches_mbase) {
		/* mbase, side1, & side2 all match; use mbase as resolution */
		setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
				names, names+0, mbase_null, 0,
				filemask, dirmask, 1);
		return mask;
	}

	/*
	 * Record information about the path so we can resolve later in
	 * process_entries.
	 */
	setup_path_info(opt, &pi, dirname, info->pathlen, fullpath,
			names, NULL, 0, df_conflict, filemask, dirmask, 0);
	ci = pi.util;
	ci->match_mask = match_mask;

	/* If dirmask, recurse into subdirectories */
	if (dirmask) {
		struct traverse_info newinfo;
		struct tree_desc t[3];
		void *buf[3] = {NULL,};
		const char *original_dir_name;
		int i, ret;

		ci->match_mask &= filemask;
		newinfo = *info;
		newinfo.prev = info;
		newinfo.name = p->path;
		newinfo.namelen = p->pathlen;
		newinfo.pathlen = st_add3(newinfo.pathlen, p->pathlen, 1);
		/*
		 * If we did care about parent directories having a D/F
		 * conflict, then we'd include
		 *    newinfo.df_conflicts |= (mask & ~dirmask);
		 * here.  But we don't.  (See comment near setting of local
		 * df_conflict variable near the beginning of this function).
		 */

		for (i = 0; i < 3; i++, dirmask >>= 1) {
			if (i == 1 && side1_matches_mbase)
				t[1] = t[0];
			else if (i == 2 && side2_matches_mbase)
				t[2] = t[0];
			else if (i == 2 && sides_match)
				t[2] = t[1];
			else {
				const struct object_id *oid = NULL;
				if (dirmask & 1)
					oid = &names[i].oid;
				buf[i] = fill_tree_descriptor(opt->repo,
							      t + i, oid);
			}
		}

		original_dir_name = opti->current_dir_name;
		opti->current_dir_name = pi.string;
		ret = traverse_trees(NULL, 3, t, &newinfo);
		opti->current_dir_name = original_dir_name;

		for (i = 0; i < 3; i++)
			free(buf[i]);

		if (ret < 0)
			return -1;
	}

	return mask;
}

static int collect_merge_info(struct merge_options *opt,
			      struct tree *merge_base,
			      struct tree *side1,
			      struct tree *side2)
{
	int ret;
	struct tree_desc t[3];
	struct traverse_info info;
	char *toplevel_dir_placeholder = "";

	opt->priv->current_dir_name = toplevel_dir_placeholder;
	setup_traverse_info(&info, toplevel_dir_placeholder);
	info.fn = collect_merge_info_callback;
	info.data = opt;
	info.show_all_errors = 1;

	parse_tree(merge_base);
	parse_tree(side1);
	parse_tree(side2);
	init_tree_desc(t+0, merge_base->buffer, merge_base->size);
	init_tree_desc(t+1, side1->buffer, side1->size);
	init_tree_desc(t+2, side2->buffer, side2->size);

	ret = traverse_trees(NULL, 3, t, &info);

	return ret;
}

static int detect_and_process_renames(struct merge_options *opt,
				      struct tree *merge_base,
				      struct tree *side1,
				      struct tree *side2)
{
	int clean = 1;

	/*
	 * Rename detection works by detecting file similarity.  Here we use
	 * a really easy-to-implement scheme: files are similar IFF they have
	 * the same filename.  Therefore, by this scheme, there are no renames.
	 *
	 * TODO: Actually implement a real rename detection scheme.
	 */
	return clean;
}

static int string_list_df_name_compare(const char *one, const char *two)
{
	int onelen = strlen(one);
	int twolen = strlen(two);
	/*
	 * Here we only care that entries for D/F conflicts are
	 * adjacent, in particular with the file of the D/F conflict
	 * appearing before files below the corresponding directory.
	 * The order of the rest of the list is irrelevant for us.
	 *
	 * To achieve this, we sort with df_name_compare and provide
	 * the mode S_IFDIR so that D/F conflicts will sort correctly.
	 * We use the mode S_IFDIR for everything else for simplicity,
	 * since in other cases any changes in their order due to
	 * sorting cause no problems for us.
	 */
	int cmp = df_name_compare(one, onelen, S_IFDIR,
				  two, twolen, S_IFDIR);
	/*
	 * Now that 'foo' and 'foo/bar' compare equal, we have to make sure
	 * that 'foo' comes before 'foo/bar'.
	 */
	if (cmp)
		return cmp;
	return onelen - twolen;
}

struct directory_versions {
	struct string_list versions;
	struct string_list offsets;
	const char *last_directory;
	unsigned last_directory_len;
};

static void write_tree(struct object_id *result_oid,
		       struct string_list *versions,
		       unsigned int offset)
{
	size_t maxlen = 0;
	unsigned int nr = versions->nr - offset;
	struct strbuf buf = STRBUF_INIT;
	struct string_list relevant_entries = STRING_LIST_INIT_NODUP;
	int i;

	/*
	 * We want to sort the last (versions->nr-offset) entries in versions.
	 * Do so by abusing the string_list API a bit: make another string_list
	 * that contains just those entries and then sort them.
	 *
	 * We won't use relevant_entries again and will let it just pop off the
	 * stack, so there won't be allocation worries or anything.
	 */
	relevant_entries.items = versions->items + offset;
	relevant_entries.nr = versions->nr - offset;
	string_list_sort(&relevant_entries);

	/* Pre-allocate some space in buf */
	for (i = 0; i < nr; i++) {
		maxlen += strlen(versions->items[offset+i].string) + 34;
	}
	strbuf_reset(&buf);
	strbuf_grow(&buf, maxlen);

	/* Write each entry out to buf */
	for (i = 0; i < nr; i++) {
		struct merged_info *mi = versions->items[offset+i].util;
		struct version_info *ri = &mi->result;
		strbuf_addf(&buf, "%o %s%c",
			    ri->mode,
			    versions->items[offset+i].string, '\0');
		strbuf_add(&buf, ri->oid.hash, the_hash_algo->rawsz);
	}

	/* Write this object file out, and record in result_oid */
	write_object_file(buf.buf, buf.len, tree_type, result_oid);
	strbuf_release(&buf);
}

static void record_entry_for_tree(struct directory_versions *dir_metadata,
				  const char *path,
				  struct conflict_info *ci)
{
	const char *basename;

	if (ci->merged.is_null)
		/* nothing to record */
		return;

	/*
	 * Note: write_completed_directories() already added
	 * entries for directories to dir_metadata->versions,
	 * so no need to handle ci->filemask == 0 again.
	 */
	if (!ci->merged.clean && !ci->filemask)
		return;

	basename = path + ci->merged.basename_offset;
	assert(strchr(basename, '/') == NULL);
	string_list_append(&dir_metadata->versions,
			   basename)->util = &ci->merged.result;
}

static void write_completed_directories(struct merge_options *opt,
					const char *new_directory_name,
					struct directory_versions *info)
{
	const char *prev_dir;
	struct merged_info *dir_info = NULL;
	unsigned int offset;
	int wrote_a_new_tree = 0;

	if (new_directory_name == info->last_directory)
		return;

	/*
	 * If we are just starting (last_directory is NULL), or last_directory
	 * is a prefix of the current directory, then we can just update
	 * last_directory and record the offset where we started this directory.
	 */
	if (info->last_directory == NULL ||
	    !strncmp(new_directory_name, info->last_directory,
		     info->last_directory_len)) {
		uintptr_t offset = info->versions.nr;

		info->last_directory = new_directory_name;
		info->last_directory_len = strlen(info->last_directory);
		string_list_append(&info->offsets,
				   info->last_directory)->util = (void*)offset;
		return;
	}

	/*
	 * At this point, ne (next entry) is within a different directory
	 * than the last entry, so we need to create a tree object for all
	 * the entries in info->versions that are under info->last_directory.
	 */
	dir_info = strmap_get(&opt->priv->paths, info->last_directory);
	assert(dir_info);
	offset = (uintptr_t)info->offsets.items[info->offsets.nr-1].util;
	if (offset == info->versions.nr) {
		dir_info->is_null = 1;
	} else {
		dir_info->result.mode = S_IFDIR;
		write_tree(&dir_info->result.oid, &info->versions, offset);
		wrote_a_new_tree = 1;
	}

	/*
	 * We've now used several entries from info->versions and one entry
	 * from info->offsets, so we get rid of those values.
	 */
	info->offsets.nr--;
	info->versions.nr = offset;

	/*
	 * Now we've got an OID for last_directory in dir_info.  We need to
	 * add it to info->versions for it to be part of the computation of
	 * its parent directories' OID.  But first, we have to find out what
	 * its' parent name was and whether that matches the previous
	 * info->offsets or we need to set up a new one.
	 */
	prev_dir = info->offsets.nr == 0 ? NULL :
		   info->offsets.items[info->offsets.nr-1].string;
	if (new_directory_name != prev_dir) {
		uintptr_t c = info->versions.nr;
		string_list_append(&info->offsets,
				   new_directory_name)->util = (void*)c;
	}

	/*
	 * Okay, finally record OID for last_directory in info->versions,
	 * and update last_directory.
	 */
	if (wrote_a_new_tree) {
		const char *dir_name = strrchr(info->last_directory, '/');
		dir_name = dir_name ? dir_name+1 : info->last_directory;
		string_list_append(&info->versions, dir_name)->util = dir_info;
	}
	info->last_directory = new_directory_name;
	info->last_directory_len = strlen(info->last_directory);
}

/* Per entry merge function */
static void process_entry(struct merge_options *opt,
			  const char *path,
			  struct conflict_info *ci,
			  struct directory_versions *dir_metadata)
{
	assert(!ci->merged.clean);
	assert(ci->filemask >= 0 && ci->filemask <= 7);

	if (ci->filemask == 0) {
		/*
		 * This is a placeholder for directories that were recursed
		 * into; nothing to do in this case.
		 */
		return;
	}

	if (ci->df_conflict) {
		die("Not yet implemented.");
	}

	/*
	 * NOTE: Below there is a long switch-like if-elseif-elseif... block
	 *       which the code goes through even for the df_conflict cases
	 *       above.  Well, it will once we don't die-not-implemented above.
	 */
	if (ci->match_mask) {
		ci->merged.clean = 1;
		if (ci->match_mask == 6) {
			/* stages[1] == stages[2] */
			ci->merged.result.mode = ci->stages[1].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
		} else {
			/* determine the mask of the side that didn't match */
			unsigned int othermask = 7 & ~ci->match_mask;
			int side = (othermask == 4) ? 2 : 1;

			ci->merged.is_null = (ci->filemask == ci->match_mask);
			ci->merged.result.mode = ci->stages[side].mode;
			oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);

			assert(othermask == 2 || othermask == 4);
			assert(ci->merged.is_null == !ci->merged.result.mode);
		}
	} else if (ci->filemask >= 6 &&
		   (S_IFMT & ci->stages[1].mode) !=
		   (S_IFMT & ci->stages[2].mode)) {
		/*
		 * Two different items from (file/submodule/symlink)
		 */
		die("Not yet implemented.");
	} else if (ci->filemask >= 6) {
		/*
		 * TODO: Needs a two-way or three-way content merge, but we're
		 * just being lazy and copying the version from HEAD and
		 * leaving it as conflicted.
		 */
		ci->merged.clean = 0;
		ci->merged.result.mode = ci->stages[1].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[1].oid);
	} else if (ci->filemask == 3 || ci->filemask == 5) {
		/* Modify/delete */
		die("Not yet implemented.");
	} else if (ci->filemask == 2 || ci->filemask == 4) {
		/* Added on one side */
		int side = (ci->filemask == 4) ? 2 : 1;
		ci->merged.result.mode = ci->stages[side].mode;
		oidcpy(&ci->merged.result.oid, &ci->stages[side].oid);
		ci->merged.clean = !ci->df_conflict && !ci->path_conflict;
	} else if (ci->filemask == 1) {
		/* Deleted on both sides */
		ci->merged.is_null = 1;
		ci->merged.result.mode = 0;
		oidcpy(&ci->merged.result.oid, &null_oid);
		ci->merged.clean = !ci->path_conflict;
	}

	/*
	 * If still unmerged, record it separately.  This allows us to later
	 * iterate over just unmerged entries when updating the index instead
	 * of iterating over all entries.
	 */
	if (!ci->merged.clean)
		strmap_put(&opt->priv->unmerged, path, ci);
	record_entry_for_tree(dir_metadata, path, ci);
}

static void process_entries(struct merge_options *opt,
			    struct object_id *result_oid)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;
	struct string_list plist = STRING_LIST_INIT_NODUP;
	struct string_list_item *entry;
	struct directory_versions dir_metadata;

	if (strmap_empty(&opt->priv->paths)) {
		oidcpy(result_oid, opt->repo->hash_algo->empty_tree);
		return;
	}

	/* Hack to pre-allocate plist to the desired size */
	ALLOC_GROW(plist.items, strmap_get_size(&opt->priv->paths), plist.alloc);

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(&opt->priv->paths, &iter, e) {
		string_list_append(&plist, e->key)->util = e->value;
	}
	plist.cmp = string_list_df_name_compare;
	string_list_sort(&plist);

	/* other setup */
	string_list_init(&dir_metadata.versions, 0);
	string_list_init(&dir_metadata.offsets, 0);
	dir_metadata.last_directory = NULL;
	dir_metadata.last_directory_len = 0;

	/*
	 * Iterate over the items in reverse order, so we can handle paths
	 * below a directory before needing to handle the directory itself.
	 */
	for (entry = &plist.items[plist.nr-1]; entry >= plist.items; --entry) {
		char *path = entry->string;
		/*
		 * WARNING: If ci->merged.clean is true, then ci does not
		 * actually point to a conflict_info but a struct merge_info.
		 */
		struct conflict_info *ci = entry->util;

		write_completed_directories(opt, ci->merged.directory_name,
					    &dir_metadata);
		if (ci->merged.clean)
			record_entry_for_tree(&dir_metadata, path, ci);
		else
			process_entry(opt, path, ci, &dir_metadata);
	}

	if (dir_metadata.offsets.nr != 1 ||
	    (uintptr_t)dir_metadata.offsets.items[0].util != 0) {
		printf("dir_metadata.offsets.nr = %d (should be 1)\n",
		       dir_metadata.offsets.nr);
		printf("dir_metadata.offsets.items[0].util = %u (should be 0)\n",
		       (unsigned)(uintptr_t)dir_metadata.offsets.items[0].util);
		fflush(stdout);
		BUG("dir_metadata accounting completely off; shouldn't happen");
	}
	write_tree(result_oid, &dir_metadata.versions, 0);
	string_list_clear(&plist, 0);
	string_list_clear(&dir_metadata.versions, 0);
	string_list_clear(&dir_metadata.offsets, 0);
}

static int checkout(struct merge_options *opt,
		    struct tree *prev,
		    struct tree *next)
{
	/* Switch the index/working copy from old to new */
	int ret;
	struct tree_desc trees[2];
	struct unpack_trees_options unpack_opts;

	memset(&unpack_opts, 0, sizeof(unpack_opts));
	unpack_opts.head_idx = -1;
	unpack_opts.src_index = opt->repo->index;
	unpack_opts.dst_index = opt->repo->index;

	setup_unpack_trees_porcelain(&unpack_opts, "merge");

	/*
	 * NOTE: if this were just "git checkout" code, we would probably
	 * read or refresh the cache and check for an unmerged index, but
	 * builtin/merge.c or sequencer.c really needs to read the index
	 * and check for unmerged entries before starting merging for a
	 * good user experience (no sense waiting for merges/rebases before
	 * erroring out), so there's no reason to duplicate that work here.
	 */

	/* 2-way merge to the new branch */
	unpack_opts.update = 1;
	unpack_opts.merge = 1;
	unpack_opts.quiet = 0; /* FIXME: sequencer might want quiet? */
	unpack_opts.verbose_update = (opt->verbosity > 2);
	unpack_opts.fn = twoway_merge;
	if (1/* FIXME: opts->overwrite_ignore*/) {
		unpack_opts.dir = xcalloc(1, sizeof(*unpack_opts.dir));
		unpack_opts.dir->flags |= DIR_SHOW_IGNORED;
		setup_standard_excludes(unpack_opts.dir);
	}
	parse_tree(prev);
	init_tree_desc(&trees[0], prev->buffer, prev->size);
	parse_tree(next);
	init_tree_desc(&trees[1], next->buffer, next->size);

	ret = unpack_trees(2, trees, &unpack_opts);
	clear_unpack_trees_porcelain(&unpack_opts);
	dir_clear(unpack_opts.dir);
	FREE_AND_NULL(unpack_opts.dir);
	return ret;
}

static int record_unmerged_index_entries(struct merge_options *opt,
					 struct index_state *index,
					 struct strmap *paths,
					 struct strmap *unmerged)
{
	struct hashmap_iter iter;
	struct strmap_entry *e;
	int errs = 0;
	int original_cache_nr;

	if (strmap_empty(unmerged))
		return 0;

	original_cache_nr = index->cache_nr;

	/* Put every entry from paths into plist, then sort */
	strmap_for_each_entry(unmerged, &iter, e) {
		const char *path = e->key;
		struct conflict_info *ci = e->value;
		int pos;
		struct cache_entry *ce;
		int i;

		/*
		 * The index will already have a stage=0 entry for this path,
		 * because we created an as-merged-as-possible version of the
		 * file and checkout() moved the working copy and index over
		 * to that version.
		 *
		 * However, previous iterations through this loop will have
		 * added unstaged entries to the end of the cache which
		 * ignore the standard alphabetical ordering of cache
		 * entries and break invariants needed for index_name_pos()
		 * to work.  However, we know the entry we want is before
		 * those appended cache entries, so do a temporary swap on
		 * cache_nr to only look through entries of interest.
		 */
		SWAP(index->cache_nr, original_cache_nr);
		pos = index_name_pos(index, path, strlen(path));
		SWAP(index->cache_nr, original_cache_nr);
		if (pos < 0) {
			if (ci->filemask == 1)
				cache_tree_invalidate_path(index, path);
			else
				BUG("Unmerged %s but nothing in basic working tree or index; this shouldn't happen", path);
		} else {
			ce = index->cache[pos];

			/*
			 * Clean paths with CE_SKIP_WORKTREE set will not be
			 * written to the working tree by the unpack_trees()
			 * call in checkout().  Our unmerged entries would
			 * have appeared clean to that code since we ignored
			 * the higher order stages.  Thus, we need override
			 * the CE_SKIP_WORKTREE bit and manually write those
			 * files to the working disk here.
			 *
			 * TODO: Implement this CE_SKIP_WORKTREE fixup.
			 */

			/*
			 * Mark this cache entry for removal and instead add
			 * new stage>0 entries corresponding to the
			 * conflicts.  If there are many unmerged entries, we
			 * want to avoid memmove'ing O(NM) entries by
			 * inserting the new entries one at a time.  So,
			 * instead, we just add the new cache entries to the
			 * end (ignoring normal index requirements on sort
			 * order) and sort the index once we're all done.
			 */
			ce->ce_flags |= CE_REMOVE;
		}

		for (i = 0; i < 3; i++) {
			struct version_info *vi;
			if (!(ci->filemask & (1ul << i)))
				continue;
			vi = &ci->stages[i];
			ce = make_cache_entry(index, vi->mode, &vi->oid,
					      path, i+1, 0);
			add_index_entry(index, ce, ADD_CACHE_JUST_APPEND);
		}
	}

	/*
	 * Remove the unused cache entries (and invalidate the relevant
	 * cache-trees), then sort the index entries to get the unmerged
	 * entries we added to the end into their right locations.
	 */
	remove_marked_cache_entries(index, 1);
	QSORT(index->cache, index->cache_nr, cmp_cache_name_compare);

	return errs;
}

void merge_switch_to_result(struct merge_options *opt,
			    struct tree *head,
			    struct merge_result *result,
			    int update_worktree_and_index,
			    int display_update_msgs)
{
	assert(opt->priv == NULL);
	if (result->clean >= 0 && update_worktree_and_index) {
		struct merge_options_internal *opti = result->priv;

		if (checkout(opt, head, result->tree)) {
			/* failure to function */
			result->clean = -1;
			return;
		}

		if (record_unmerged_index_entries(opt, opt->repo->index,
						  &opti->paths,
						  &opti->unmerged)) {
			/* failure to function */
			result->clean = -1;
			return;
		}
	}

	if (display_update_msgs) {
		/* TODO: print out CONFLICT and other informational messages. */
	}

	merge_finalize(opt, result);
}

void merge_finalize(struct merge_options *opt,
		    struct merge_result *result)
{
	struct merge_options_internal *opti = result->priv;

	assert(opt->priv == NULL);

	/*
	 * We marked opti->paths with strdup_strings = 0, so that we
	 * wouldn't have to make another copy of the fullpath created by
	 * make_traverse_path from setup_path_info().  But, now that we've
	 * used it and have no other references to these strings, it is time
	 * to deallocate them.
	 */
	free_strmap_strings(&opti->paths);
	strmap_clear(&opti->paths, 1);

	/*
	 * All strings and util fields in opti->unmerged are a subset of
	 * those in opti->paths.  We don't want to deallocate anything
	 * twice, so we don't free the strings we pass 0 for free_util.
	 */
	strmap_clear(&opti->unmerged, 0);
	FREE_AND_NULL(opti);
}

static void merge_start(struct merge_options *opt, struct merge_result *result)
{
	/* Sanity checks on opt */
	assert(opt->repo);

	assert(opt->branch1 && opt->branch2);

	assert(opt->detect_directory_renames >= MERGE_DIRECTORY_RENAMES_NONE &&
	       opt->detect_directory_renames <= MERGE_DIRECTORY_RENAMES_TRUE);
	assert(opt->rename_limit >= -1);
	assert(opt->rename_score >= 0 && opt->rename_score <= MAX_SCORE);
	assert(opt->show_rename_progress >= 0 && opt->show_rename_progress <= 1);

	assert(opt->xdl_opts >= 0);
	assert(opt->recursive_variant >= MERGE_VARIANT_NORMAL &&
	       opt->recursive_variant <= MERGE_VARIANT_THEIRS);

	/*
	 * detect_renames, verbosity, buffer_output, and obuf are ignored
	 * fields that were used by "recursive" rather than "ort" -- but
	 * sanity check them anyway.
	 */
	assert(opt->detect_renames >= -1 &&
	       opt->detect_renames <= DIFF_DETECT_COPY);
	assert(opt->verbosity >= 0 && opt->verbosity <= 5);
	assert(opt->buffer_output <= 2);
	assert(opt->obuf.len == 0);

	assert(opt->priv == NULL);

	/* Default to histogram diff.  Actually, just hardcode it...for now. */
	opt->xdl_opts = DIFF_WITH_ALG(opt, HISTOGRAM_DIFF);

	/* Initialization of opt->priv, our internal merge data */
	opt->priv = xcalloc(1, sizeof(*opt->priv));
	/*
	 * Although we initialize opt->priv->paths with strdup_strings=0,
	 * that's just to avoid making yet another copy of an allocated
	 * string.  Putting the entry into paths means we are taking
	 * ownership, so we will later free it.
	 *
	 * In contrast, unmerged just has a subset of keys from paths, so
	 * we don't want to free those (it'd be a duplicate free).
	 */
	strmap_init_with_options(&opt->priv->paths, NULL, 0);
	strmap_init_with_options(&opt->priv->unmerged, NULL, 0);
}

/*
 * Originally from merge_trees_internal(); heavily adapted, though.
 */
static void merge_ort_nonrecursive_internal(struct merge_options *opt,
					    struct tree *merge_base,
					    struct tree *side1,
					    struct tree *side2,
					    struct merge_result *result)
{
	struct object_id working_tree_oid;

	if (collect_merge_info(opt, merge_base, side1, side2) != 0) {
		err(opt, _("collecting merge info failed for trees %s, %s, %s"),
		    oid_to_hex(&merge_base->object.oid),
		    oid_to_hex(&side1->object.oid),
		    oid_to_hex(&side2->object.oid));
		result->clean = -1;
		return;
	}

	result->clean = detect_and_process_renames(opt, merge_base,
						   side1, side2);
	process_entries(opt, &working_tree_oid);

	/* Set return values */
	result->tree = parse_tree_indirect(&working_tree_oid);
	/* existence of unmerged entries implies unclean */
	result->clean &= strmap_empty(&opt->priv->unmerged);
	if (!opt->priv->call_depth) {
		result->priv = opt->priv;
		opt->priv = NULL;
	}
}

void merge_incore_nonrecursive(struct merge_options *opt,
			       struct tree *merge_base,
			       struct tree *side1,
			       struct tree *side2,
			       struct merge_result *result)
{
	assert(opt->ancestor != NULL);
	merge_start(opt, result);
	merge_ort_nonrecursive_internal(opt, merge_base, side1, side2, result);
}

void merge_incore_recursive(struct merge_options *opt,
			    struct commit_list *merge_bases,
			    struct commit *side1,
			    struct commit *side2,
			    struct merge_result *result)
{
	die("Not yet implemented");
}
