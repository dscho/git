#include "reftable/system.h"
#include "reftable/reftable-error.h"
#include "reftable/reftable-generic.h"
#include "reftable/reftable-merged.h"
#include "reftable/reftable-reader.h"
#include "reftable/reftable-stack.h"
#include "reftable/reftable-tests.h"
#include "test-tool.h"

int cmd__reftable(int argc, const char **argv)
{
	/* test from simple to complex. */
	block_test_main(argc, argv);
	tree_test_main(argc, argv);
	pq_test_main(argc, argv);
	readwrite_test_main(argc, argv);
	stack_test_main(argc, argv);
	return 0;
}

static void print_help(void)
{
	printf("usage: dump [-st] arg\n\n"
	       "options: \n"
	       "  -b dump blocks\n"
	       "  -t dump table\n"
	       "  -s dump stack\n"
	       "  -6 sha256 hash format\n"
	       "  -h this help\n"
	       "\n");
}

static char hexdigit(int c)
{
	if (c <= 9)
		return '0' + c;
	return 'a' + (c - 10);
}

static void hex_format(char *dest, const unsigned char *src, int hash_size)
{
	assert(hash_size > 0);
	if (src) {
		int i = 0;
		for (i = 0; i < hash_size; i++) {
			dest[2 * i] = hexdigit(src[i] >> 4);
			dest[2 * i + 1] = hexdigit(src[i] & 0xf);
		}
		dest[2 * hash_size] = 0;
	}
}

static int dump_table(struct reftable_table *tab)
{
	struct reftable_iterator it = { NULL };
	struct reftable_ref_record ref = { NULL };
	struct reftable_log_record log = { NULL };
	uint32_t hash_id = reftable_table_hash_id(tab);
	int hash_len = hash_size(hash_id);
	int err;

	reftable_table_init_ref_iter(tab, &it);

	err = reftable_iterator_seek_ref(&it, "");
	if (err < 0)
		return err;

	while (1) {
		char hex[GIT_MAX_HEXSZ + 1] = { 0 }; /* BUG */

		err = reftable_iterator_next_ref(&it, &ref);
		if (err > 0)
			break;
		if (err < 0)
			return err;

		printf("ref{%s(%" PRIu64 ") ", ref.refname, ref.update_index);
		switch (ref.value_type) {
		case REFTABLE_REF_SYMREF:
			printf("=> %s", ref.value.symref);
			break;
		case REFTABLE_REF_VAL2:
			hex_format(hex, ref.value.val2.value, hash_len);
			printf("val 2 %s", hex);
			hex_format(hex, ref.value.val2.target_value,
				   hash_len);
			printf("(T %s)", hex);
			break;
		case REFTABLE_REF_VAL1:
			hex_format(hex, ref.value.val1, hash_len);
			printf("val 1 %s", hex);
			break;
		case REFTABLE_REF_DELETION:
			printf("delete");
			break;
		}
		printf("}\n");
	}
	reftable_iterator_destroy(&it);
	reftable_ref_record_release(&ref);

	reftable_table_init_log_iter(tab, &it);

	err = reftable_iterator_seek_log(&it, "");
	if (err < 0)
		return err;

	while (1) {
		char hex[GIT_MAX_HEXSZ + 1] = { 0 };

		err = reftable_iterator_next_log(&it, &log);
		if (err > 0)
			break;
		if (err < 0)
			return err;

		switch (log.value_type) {
		case REFTABLE_LOG_DELETION:
			printf("log{%s(%" PRIu64 ") delete\n", log.refname,
			       log.update_index);
			break;
		case REFTABLE_LOG_UPDATE:
			printf("log{%s(%" PRIu64 ") %s <%s> %" PRIu64 " %04d\n",
			       log.refname, log.update_index,
			       log.value.update.name ? log.value.update.name : "",
			       log.value.update.email ? log.value.update.email : "",
			       log.value.update.time,
			       log.value.update.tz_offset);
			hex_format(hex, log.value.update.old_hash, hash_len);
			printf("%s => ", hex);
			hex_format(hex, log.value.update.new_hash, hash_len);
			printf("%s\n\n%s\n}\n", hex,
			       log.value.update.message ? log.value.update.message : "");
			break;
		}
	}
	reftable_iterator_destroy(&it);
	reftable_log_record_release(&log);
	return 0;
}

static int dump_stack(const char *stackdir, uint32_t hash_id)
{
	struct reftable_stack *stack = NULL;
	struct reftable_write_options opts = { .hash_id = hash_id };
	struct reftable_merged_table *merged = NULL;
	struct reftable_table table = { NULL };

	int err = reftable_new_stack(&stack, stackdir, &opts);
	if (err < 0)
		goto done;

	merged = reftable_stack_merged_table(stack);
	reftable_table_from_merged_table(&table, merged);
	err = dump_table(&table);
done:
	if (stack)
		reftable_stack_destroy(stack);
	return err;
}

static int dump_reftable(const char *tablename)
{
	struct reftable_block_source src = { NULL };
	int err = reftable_block_source_from_file(&src, tablename);
	struct reftable_reader *r = NULL;
	struct reftable_table tab = { NULL };
	if (err < 0)
		goto done;

	err = reftable_new_reader(&r, &src, tablename);
	if (err < 0)
		goto done;

	reftable_table_from_reader(&tab, r);
	err = dump_table(&tab);
done:
	reftable_reader_free(r);
	return err;
}

int cmd__dump_reftable(int argc, const char **argv)
{
	int err = 0;
	int opt_dump_blocks = 0;
	int opt_dump_table = 0;
	int opt_dump_stack = 0;
	uint32_t opt_hash_id = GIT_SHA1_FORMAT_ID;
	const char *arg = NULL, *argv0 = argv[0];

	for (; argc > 1; argv++, argc--)
		if (*argv[1] != '-')
			break;
		else if (!strcmp("-b", argv[1]))
			opt_dump_blocks = 1;
		else if (!strcmp("-t", argv[1]))
			opt_dump_table = 1;
		else if (!strcmp("-6", argv[1]))
			opt_hash_id = GIT_SHA256_FORMAT_ID;
		else if (!strcmp("-s", argv[1]))
			opt_dump_stack = 1;
		else if (!strcmp("-?", argv[1]) || !strcmp("-h", argv[1])) {
			print_help();
			return 2;
		}

	if (argc != 2) {
		fprintf(stderr, "need argument\n");
		print_help();
		return 2;
	}

	arg = argv[1];

	if (opt_dump_blocks) {
		err = reftable_reader_print_blocks(arg);
	} else if (opt_dump_table) {
		err = dump_reftable(arg);
	} else if (opt_dump_stack) {
		err = dump_stack(arg, opt_hash_id);
	}

	if (err < 0) {
		fprintf(stderr, "%s: %s: %s\n", argv0, arg,
			reftable_error_str(err));
		return 1;
	}
	return 0;
}
