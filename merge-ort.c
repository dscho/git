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

#include "strmap.h"
#include "tree.h"

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

static int collect_merge_info(struct merge_options *opt,
			      struct tree *merge_base,
			      struct tree *side1,
			      struct tree *side2)
{
	die("Not yet implemented.");
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

static void process_entries(struct merge_options *opt,
			    struct object_id *result_oid)
{
	die("Not yet implemented.");
}

void merge_switch_to_result(struct merge_options *opt,
			    struct tree *head,
			    struct merge_result *result,
			    int update_worktree_and_index,
			    int display_update_msgs)
{
	die("Not yet implemented");
	merge_finalize(opt, result);
}

void merge_finalize(struct merge_options *opt,
		    struct merge_result *result)
{
	die("Not yet implemented");
}

static void merge_start(struct merge_options *opt, struct merge_result *result)
{
	die("Not yet implemented.");
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

	collect_merge_info(opt, merge_base, side1, side2);
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
