#include "cache.h"
#include "range-diff.h"
#include "string-list.h"
#include "run-command.h"
#include "argv-array.h"
#include "hashmap.h"
#include "xdiff-interface.h"
#include "linear-assignment.h"

struct patch_util {
	/* For the search for an exact match */
	struct hashmap_entry e;
	const char *diff, *patch;

	int i;
	int diffsize;
	size_t diff_offset;
	/* the index of the matching item in the other branch, or -1 */
	int matching;
	struct object_id oid;
};

/*
 * Reads the patches into a string list, with the `util` field being populated
 * as struct object_id (will need to be free()d).
 */
static int read_patches(const char *range, struct string_list *list)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	FILE *in;
	struct strbuf buf = STRBUF_INIT, line = STRBUF_INIT;
	struct patch_util *util = NULL;
	int in_header = 1;

	argv_array_pushl(&cp.args, "log", "--no-color", "-p", "--no-merges",
			"--reverse", "--date-order", "--decorate=no",
			"--no-abbrev-commit", range,
			NULL);
	cp.out = -1;
	cp.no_stdin = 1;
	cp.git_cmd = 1;

	if (start_command(&cp))
		return error_errno(_("could not start `log`"));
	in = fdopen(cp.out, "r");
	if (!in) {
		error_errno(_("could not read `log` output"));
		finish_command(&cp);
		return -1;
	}

	while (strbuf_getline(&line, in) != EOF) {
		const char *p;

		if (skip_prefix(line.buf, "commit ", &p)) {
			if (util) {
				string_list_append(list, buf.buf)->util = util;
				strbuf_reset(&buf);
			}
			util = xcalloc(sizeof(*util), 1);
			if (get_oid(p, &util->oid)) {
				error(_("could not parse commit '%s'"), p);
				free(util);
				string_list_clear(list, 1);
				strbuf_release(&buf);
				strbuf_release(&line);
				fclose(in);
				finish_command(&cp);
				return -1;
			}
			util->matching = -1;
			in_header = 1;
			continue;
		}

		if (starts_with(line.buf, "diff --git")) {
			in_header = 0;
			strbuf_addch(&buf, '\n');
			if (!util->diff_offset)
				util->diff_offset = buf.len;
			strbuf_addbuf(&buf, &line);
		} else if (in_header) {
			if (starts_with(line.buf, "Author: ")) {
				strbuf_addbuf(&buf, &line);
				strbuf_addstr(&buf, "\n\n");
			} else if (starts_with(line.buf, "    ")) {
				strbuf_addbuf(&buf, &line);
				strbuf_addch(&buf, '\n');
			}
			continue;
		} else if (starts_with(line.buf, "@@ "))
			strbuf_addstr(&buf, "@@");
		else if (line.buf[0] && !starts_with(line.buf, "index "))
			/*
			 * A completely blank (not ' \n', which is context)
			 * line is not valid in a diff.  We skip it
			 * silently, because this neatly handles the blank
			 * separator line between commits in git-log
			 * output.
			 */
			strbuf_addbuf(&buf, &line);
		else
			continue;

		strbuf_addch(&buf, '\n');
		util->diffsize++;
	}
	fclose(in);
	strbuf_release(&line);

	if (util)
		string_list_append(list, buf.buf)->util = util;
	strbuf_release(&buf);

	if (finish_command(&cp))
		return -1;

	return 0;
}

static int patch_util_cmp(const void *dummy, const struct patch_util *a,
		     const struct patch_util *b, const char *keydata)
{
	return strcmp(a->diff, keydata ? keydata : b->diff);
}

static void find_exact_matches(struct string_list *a, struct string_list *b)
{
	struct hashmap map;
	int i;

	hashmap_init(&map, (hashmap_cmp_fn)patch_util_cmp, NULL, 0);

	/* First, add the patches of a to a hash map */
	for (i = 0; i < a->nr; i++) {
		struct patch_util *util = a->items[i].util;

		util->i = i;
		util->patch = a->items[i].string;
		util->diff = util->patch + util->diff_offset;
		hashmap_entry_init(util, strhash(util->diff));
		hashmap_add(&map, util);
	}

	/* Now try to find exact matches in b */
	for (i = 0; i < b->nr; i++) {
		struct patch_util *util = b->items[i].util, *other;

		util->i = i;
		util->patch = b->items[i].string;
		util->diff = util->patch + util->diff_offset;
		hashmap_entry_init(util, strhash(util->diff));
		other = hashmap_remove(&map, util, NULL);
		if (other) {
			if (other->matching >= 0)
				BUG("already assigned!");

			other->matching = i;
			util->matching = other->i;
		}
	}

	hashmap_free(&map, 0);
}

static void diffsize_consume(void *data, char *line, unsigned long len)
{
	(*(int *)data)++;
}

static int diffsize(const char *a, const char *b)
{
	xpparam_t pp = { 0 };
	xdemitconf_t cfg = { 0 };
	mmfile_t mf1, mf2;
	int count = 0;

	mf1.ptr = (char *)a;
	mf1.size = strlen(a);
	mf2.ptr = (char *)b;
	mf2.size = strlen(b);

	cfg.ctxlen = 3;
	if (!xdi_diff_outf(&mf1, &mf2, diffsize_consume, &count, &pp, &cfg))
		return count;

	error(_("failed to generate diff"));
	return COST_MAX;
}

static void get_correspondences(struct string_list *a, struct string_list *b,
				int creation_factor)
{
	int n = a->nr + b->nr;
	int *cost, c, *a2b, *b2a;
	int i, j;

	ALLOC_ARRAY(cost, st_mult(n, n));
	ALLOC_ARRAY(a2b, n);
	ALLOC_ARRAY(b2a, n);

	for (i = 0; i < a->nr; i++) {
		struct patch_util *a_util = a->items[i].util;

		for (j = 0; j < b->nr; j++) {
			struct patch_util *b_util = b->items[j].util;

			if (a_util->matching == j)
				c = 0;
			else if (a_util->matching < 0 && b_util->matching < 0)
				c = diffsize(a_util->diff, b_util->diff);
			else
				c = COST_MAX;
			cost[i + n * j] = c;
		}

		c = a_util->matching < 0 ?
			a_util->diffsize * creation_factor / 100 : COST_MAX;
		for (j = b->nr; j < n; j++)
			cost[i + n * j] = c;
	}

	for (j = 0; j < b->nr; j++) {
		struct patch_util *util = b->items[j].util;

		c = util->matching < 0 ?
			util->diffsize * creation_factor / 100 : COST_MAX;
		for (i = a->nr; i < n; i++)
			cost[i + n * j] = c;
	}

	for (i = a->nr; i < n; i++)
		for (j = b->nr; j < n; j++)
			cost[i + n * j] = 0;

	compute_assignment(n, n, cost, a2b, b2a);

	for (i = 0; i < a->nr; i++)
		if (a2b[i] >= 0 && a2b[i] < b->nr) {
			struct patch_util *a_util = a->items[i].util;
			struct patch_util *b_util = b->items[a2b[i]].util;

			a_util->matching = a2b[i];
			b_util->matching = i;
		}

	free(cost);
	free(a2b);
	free(b2a);
}

static const char *short_oid(struct patch_util *util)
{
	return find_unique_abbrev(util->oid.hash, DEFAULT_ABBREV);
}

static void output(struct string_list *a, struct string_list *b)
{
	int i;

	for (i = 0; i < b->nr; i++) {
		struct patch_util *util = b->items[i].util, *prev;

		if (util->matching < 0)
			printf("-: -------- > %d: %s\n",
					i + 1, short_oid(util));
		else {
			prev = a->items[util->matching].util;
			printf("%d: %s ! %d: %s\n",
			       util->matching + 1, short_oid(prev),
			       i + 1, short_oid(util));
		}
	}

	for (i = 0; i < a->nr; i++) {
		struct patch_util *util = a->items[i].util;

		if (util->matching < 0)
			printf("%d: %s < -: --------\n",
			       i + 1, short_oid(util));
	}
}

int show_range_diff(const char *range1, const char *range2,
		    int creation_factor)
{
	int res = 0;

	struct string_list branch1 = STRING_LIST_INIT_DUP;
	struct string_list branch2 = STRING_LIST_INIT_DUP;

	if (read_patches(range1, &branch1))
		res = error(_("could not parse log for '%s'"), range1);
	if (!res && read_patches(range2, &branch2))
		res = error(_("could not parse log for '%s'"), range2);

	if (!res) {
		find_exact_matches(&branch1, &branch2);
		get_correspondences(&branch1, &branch2, creation_factor);
		output(&branch1, &branch2);
	}

	string_list_clear(&branch1, 1);
	string_list_clear(&branch2, 1);

	return res;
}
