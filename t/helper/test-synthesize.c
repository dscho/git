#include "test-tool.h"
#include "git-compat-util.h"
#include "setup.h"
#include "object-store-ll.h"
#include "read-cache-ll.h"
#include "unpack-trees.h"
#include "object-name.h"
#include "cache-tree.h"
#include "blob.h"
#include "commit.h"
#include "hex.h"
#include "strbuf.h"
#include "parse-options.h"
#include "strmap.h"

/*
 * This helper generates artificial repositories. To do so, it uses a
 * deterministic pseudo random number generator to generate artificial content
 * and content changes.
 *
 * Please note that true randomness (or even cryptigraphically strong) is not
 * required.
 *
 * The following deterministic pseudo-random number generator was adapted from
 * the public domain code on http://burtleburtle.net/bob/rand/smallprng.html.
 */
struct random_context {
	uint32_t a, b, c, d;
};

static inline uint32_t rot(uint32_t x, int bits)
{
	return x << bits | x >> (32 - bits);
}

static uint32_t random_value(struct random_context *context)
{
	uint32_t e = context->a - rot(context->b, 27);
	context->a = context->b ^ rot(context->c, 17);
	context->b = context->c + context->d;
	context->c = context->d + e;
	context->d = e + context->a;
	return context->d;
}

/*
 * Returns a random number in the range 0, ..., range - 1.
 */
static int random_value_in_range(struct random_context *context, int range)
{
	return (int)(random_value(context) *
		     (uint64_t)range / (UINT_MAX + (uint64_t)1));
}

static void random_init(struct random_context *context, uint32_t seed)
{
	int i;

	context->a = 0xf1ea5eed;
	context->b = context->c = context->d = seed;

	for (i = 0; i < 20; i++)
		(void) random_value(context);
}

/*
 * Relatively stupid, but fun, code to generate file content that looks like
 * text in some foreign language.
 */
static void random_word(struct random_context *context, struct strbuf *buf)
{
	static char vowels[] = {
		'a', 'e', 'e', 'i', 'i', 'o', 'u', 'y'
	};
	static char consonants[] = {
		'b', 'c', 'd', 'f', 'g', 'h', 'k', 'l',
		'm', 'n', 'p', 'r', 's', 't', 'v', 'z'
	};
	int syllable_count = 1 + (random_value(context) & 0x3);

	while (syllable_count--) {
		strbuf_addch(buf, consonants[random_value(context) & 0xf]);
		strbuf_addch(buf, vowels[random_value(context) & 0x7]);
	}
}

static void random_sentence(struct random_context *context, struct strbuf *buf)
{
	int word_count = 2 + (random_value(context) & 0xf);

	if (buf->len && buf->buf[buf->len - 1] != '\n')
		strbuf_addch(buf, ' ');

	while (word_count--) {
		random_word(context, buf);
		strbuf_addch(buf, word_count ? ' ' : '.');
	}
}

static void random_paragraph(struct random_context *context, struct strbuf *buf)
{
	int sentence_count = 1 + (random_value(context) & 0x7);

	if (buf->len)
		strbuf_addstr(buf, "\n\n");

	while (sentence_count--)
		random_sentence(context, buf);
}

static void random_content(struct random_context *context, struct strbuf *buf)
{
	int paragraph_count = 1 + (random_value(context) & 0x7);

	while (paragraph_count--)
		random_paragraph(context, buf);
}

/*
 * Relatively stupid, but fun, simulation of what software developers do all
 * day long: change files, add files, occasionally remove files.
 */
static const char *random_new_path(struct random_context *context,
				   struct index_state *istate,
				   struct strbuf *buf)
{
	int pos, count, slash, i;
	const char *name, *slash_p;

	strbuf_reset(buf);
	if (!istate->cache_nr) {
		random_word(context, buf);
		return buf->buf;
	}

	pos = random_value_in_range(context, istate->cache_nr);
	name = istate->cache[pos]->name;

	/* determine number of files in the same directory */
	slash_p = strrchr(name, '/');
	slash = slash_p ? slash_p - name + 1 : 0;
	strbuf_add(buf, name, slash);
	count = 1;
	for (i = pos; i > 0; i--)
		if (strncmp(istate->cache[i - 1]->name, name, slash))
			break;
		else if (!strchr(istate->cache[i - 1]->name + slash, '/'))
			count++;
	for (i = pos + 1; i < istate->cache_nr; i++)
		if (strncmp(istate->cache[i]->name, name, slash))
			break;
		else if (!strchr(istate->cache[i]->name + slash, '/'))
			count++;

	/* Depending how many files there are already, add a new directory */
	if (random_value_in_range(context, 20) < count) {
		int len = buf->len;
		for (;;) {
			strbuf_setlen(buf, len);
			random_word(context, buf);
			/* Avoid clashes with existing files or directories */
			i = index_name_pos(istate, buf->buf, buf->len);
			if (i >= 0)
				continue;
			strbuf_addch(buf, '/');
			i = -1 - i;
			if (i >= istate->cache_nr ||
			    !starts_with(istate->cache[i]->name, buf->buf))
				break;
		}
	}

	/* Make sure that the new path is, in fact, new */
	i = buf->len;
	for (;;) {
		int pos;

		random_word(context, buf);
		pos = index_name_pos(istate, buf->buf, buf->len);
		if (pos < 0) {
			/* Make sure that we do not clash with a directory */
			for (pos = -1 - pos; pos < istate->cache_nr; pos++) {
				const char *name = istate->cache[pos]->name;
				char c;

				if (!starts_with(name, buf->buf))
					return buf->buf;
				c = name[buf->len];
				if (c > '/')
					return buf->buf;
				if (c == '/')
					break;
			}
			if (pos == istate->cache_nr)
				return buf->buf;
		}
		strbuf_setlen(buf, i);
	}
}

static void modify_randomly(struct random_context *context, struct strbuf *buf)
{
	int count = 1 + random_value_in_range(context, 5);
	struct strbuf replace = STRBUF_INIT;

	while (count--) {
		int pos = random_value_in_range(context, buf->len);
		const char *eol = strchrnul(buf->buf + pos, '\n');
		int end_pos = pos + random_value_in_range(context,
			eol - buf->buf - pos);
		int new_count;

		while (pos && !isspace(buf->buf[pos - 1]))
			pos--;
		while (end_pos < buf->len && !isspace(buf->buf[end_pos]))
			end_pos++;

		new_count = !pos + random_value_in_range(context,
			(end_pos - pos) / 3);
		/* Do not simply delete ends of paragraphs. */
		if (!new_count && (buf->buf[end_pos] == '\n' ||
				   end_pos == buf->len))
			new_count++;
		strbuf_reset(&replace);
		while (new_count--) {
			if (replace.len)
				strbuf_addch(&replace, ' ');
			random_word(context, &replace);
		}
		if (buf->buf[end_pos] == '\n' || end_pos == buf->len)
			strbuf_addch(&replace, '.');
		strbuf_splice(buf, pos, end_pos - pos,
			      replace.buf, replace.len);
	}

	strbuf_release(&replace);
}

static int random_work(struct repository *r,
		       struct random_context *context,
		       struct index_state *istate)
{
	int count, delete = 0, add = 0, modify;
	struct strset touched_path;

	/* Obey a totally made-up distribution how many files to remove */
	if (istate->cache_nr > 20) {
		delete = !random_value_in_range(context, 40);
		if (!random_value_in_range(context, 100))
			delete++;
	}

	/* Obey a totally made-up distribution how many files to add */
	add = !istate->cache_nr;
	if (!random_value_in_range(context, istate->cache_nr < 5 ? 2 : 5)) {
		add++;
		if (!random_value_in_range(context, 2)) {
			add++;
			if (!random_value_in_range(context, 3))
				add++;
		}
	}

	/* Obey a totally made-up distribution how many files to modify */
	modify = !(delete + add) + random_value_in_range(context, 5);
	if (modify > istate->cache_nr - delete)
		modify = istate->cache_nr - delete;

	strset_init_with_options(&touched_path, NULL, 0);

	count = delete;
	while (count--) {
		int pos = random_value_in_range(context, istate->cache_nr);

		strset_add(&touched_path, istate->cache[pos]->name);

		cache_tree_invalidate_path(istate, istate->cache[pos]->name);
		remove_index_entry_at(istate, pos);
	}

	count = modify;
	while (count--) {
		int pos = random_value_in_range(context, istate->cache_nr);
		enum object_type type;
		unsigned long sz;
		struct strbuf buf;

		while (strset_contains(&touched_path, istate->cache[pos]->name))
			pos = random_value_in_range(context, istate->cache_nr);
		strset_add(&touched_path, istate->cache[pos]->name);

		buf.buf = repo_read_object_file(r, &istate->cache[pos]->oid, &type, &sz);
		buf.alloc = buf.len = sz;
		strbuf_grow(&buf, 0);
		buf.buf[buf.len] = '\0';

		modify_randomly(context, &buf);
		write_object_file(buf.buf, buf.len, OBJ_BLOB, &istate->cache[pos]->oid);
		cache_tree_invalidate_path(istate, istate->cache[pos]->name);
		strbuf_release(&buf);
	}

	count = add;
	while (count--) {
		struct strbuf path_buf = STRBUF_INIT, buf = STRBUF_INIT;
		const char *path = random_new_path(context, istate, &path_buf);
		struct object_id oid;
		struct cache_entry *ce;

		while (strset_contains(&touched_path, path))
			path = random_new_path(context, istate, &path_buf);
		strset_add(&touched_path, path);

		random_content(context, &buf);
		write_object_file(buf.buf, buf.len, OBJ_BLOB, &oid);
		ce = make_cache_entry(istate, 0644, &oid, path, 0, 0);
		if (!ce || add_index_entry(istate, ce, ADD_CACHE_OK_TO_ADD))
			return error("Could not add %s", path);
		strbuf_release(&path_buf);
		strbuf_release(&buf);
	}

	return 0;
}

static void random_commit_message(struct random_context *context,
				  struct strbuf *buf)
{
	int count = 3 + random_value_in_range(context, 4);

	while (count--) {
		if (buf->len)
			strbuf_addch(buf, ' ');
		random_word(context, buf);
	}

	count = random_value_in_range(context, 5);
	while (count--)
		random_paragraph(context, buf);
}

static int random_branch(struct repository *r,
			 struct random_context *context,
			 const char *start_revision,
			 int file_count_goal,
			 struct object_id *oid,
			 int show_progress)
{
	struct index_state istate;
	struct commit_list *parents = NULL;
	uint64_t tick = 1234567890ul, count = 0;
	struct strbuf msg = STRBUF_INIT, date = STRBUF_INIT;

	index_state_init(&istate, r);

	setenv("GIT_AUTHOR_NAME", "A U Thor", 1);
	setenv("GIT_AUTHOR_EMAIL", "author@email.com", 1);
	setenv("GIT_COMMITTER_NAME", "C O M Mitter", 1);
	setenv("GIT_COMMITTER_EMAIL", "committer@mitter.com", 1);

	if (!start_revision)
		istate.cache_tree = cache_tree();
	else {
		struct commit *commit;
		struct object_id tree_oid;
		struct tree *tree;
		struct unpack_trees_options opts = { 0 };
		struct tree_desc t;

		if (repo_get_oid_committish(r, start_revision, oid))
			return error("could not parse as commit '%s'", start_revision);

		commit = lookup_commit(r, oid);
		if (!commit || repo_parse_commit(r, commit) < 0)
			return error("could not parse commit '%s'", start_revision);
		tick = commit->date + random_value_in_range(context, 86400 * 3);

		if (repo_get_oid_treeish(r, start_revision, &tree_oid))
			return error("could not parse as tree '%s'", start_revision);
		tree = parse_tree_indirect(&tree_oid);

		opts.index_only = 1;
		opts.head_idx = -1;
		opts.src_index = &istate;
		opts.dst_index = &istate;
		opts.fn = oneway_merge;
		init_tree_desc(&t, tree->buffer, tree->size);
		if (unpack_trees(1, &t, &opts))
			return error("could not read %s into index", start_revision);
	}

	while (istate.cache_nr < file_count_goal) {
		if (show_progress)
			fprintf(stderr, "#%" PRIuMAX ": %" PRIuMAX "/%" PRIuMAX "\r",
				(uintmax_t)count, (uintmax_t)istate.cache_nr, (uintmax_t)file_count_goal);
		if (random_work(r, context, &istate) < 0)
			return -1;

		if (count > 0 || start_revision) {
			parents = NULL;
			commit_list_insert(lookup_commit(r, oid), &parents);
			strbuf_reset(&msg);
		}
		random_commit_message(context, &msg);

		strbuf_reset(&date);
		strbuf_addf(&date, "@%" PRIu64 " -0400", tick);
		setenv("GIT_COMMITTER_DATE", date.buf, 1);
		setenv("GIT_AUTHOR_DATE", date.buf, 1);
		tick += 60 + random_value_in_range(context, 86400 * 3);

		if (cache_tree_update(&istate, 0) ||
		    commit_tree(msg.buf, msg.len, &istate.cache_tree->oid,
				parents, oid, NULL, NULL))
			return error("Could not commit (parent: %s)",
				     parents ? oid_to_hex(oid) : "(none)");
		count++;
	}

	return 0;
}

static int cmd__synthesize__commits(int argc, const char **argv, const char *prefix UNUSED)
{
	struct repository *r = the_repository;
	struct random_context context;
	struct object_id oid;
	int seed = 123, target_file_count = 50, show_progress = isatty(2);
	const char *start_revision = NULL;
	const char * const usage[] = { argv[0], NULL };
	struct option options[] = {
		OPT_STRING('s', "start-revision", &start_revision, "revision",
			   "branch off at this revision (optional)"),
		OPT_INTEGER(0, "seed", &seed,
			"seed number for the pseudo-random number generator"),
		OPT_INTEGER(0, "target-file-count", &target_file_count,
			"stop generating revisions at this number of files"),
		OPT_BOOL(0, "progress", &show_progress, "show progress"),
		OPT_END(),
	};

	argc = parse_options(argc, (const char **)argv,
		NULL, options, usage, 0);
	if (argc)
		usage_with_options(usage, options);

	setup_git_directory();
	random_init(&context, seed);
	if (random_branch(r, &context, start_revision, target_file_count, &oid, show_progress) < 0)
		return -1;

	printf("%s", oid_to_hex(&oid));

	return 0;
}

int cmd__synthesize(int argc, const char **argv)
{
	const char *prefix = NULL;
	char const * const synthesize_usage[] = {
		"test-tool synthesize commits <options>",
		NULL,
	};
	parse_opt_subcommand_fn *fn = NULL;
	struct option options[] = {
		OPT_SUBCOMMAND("commits", &fn, cmd__synthesize__commits),
		OPT_END()
	};
	argc = parse_options(argc, argv, prefix, options, synthesize_usage, 0);
	return !!fn(argc, argv, prefix);
}
