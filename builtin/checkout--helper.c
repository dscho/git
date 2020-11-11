#include "builtin.h"
#include "config.h"
#include "entry.h"
#include "parallel-checkout.h"
#include "parse-options.h"
#include "pkt-line.h"

static void packet_to_pc_item(char *line, int len,
			      struct parallel_checkout_item *pc_item)
{
	struct pc_item_fixed_portion *fixed_portion;
	char *encoding, *variant;

	if (len < sizeof(struct pc_item_fixed_portion))
		BUG("checkout worker received too short item (got %dB, exp %dB)",
		    len, (int)sizeof(struct pc_item_fixed_portion));

	fixed_portion = (struct pc_item_fixed_portion *)line;

	if (len - sizeof(struct pc_item_fixed_portion) !=
		fixed_portion->name_len + fixed_portion->working_tree_encoding_len)
		BUG("checkout worker received corrupted item");

	variant = line + sizeof(struct pc_item_fixed_portion);

	/*
	 * Note: the main process uses zero length to communicate that the
	 * encoding is NULL. There is no use case in actually sending an empty
	 * string since it's considered as NULL when ca.working_tree_encoding
	 * is set at git_path_check_encoding().
	 */
	if (fixed_portion->working_tree_encoding_len) {
		encoding = xmemdupz(variant,
				    fixed_portion->working_tree_encoding_len);
		variant += fixed_portion->working_tree_encoding_len;
	} else {
		encoding = NULL;
	}

	memset(pc_item, 0, sizeof(*pc_item));
	pc_item->ce = make_empty_transient_cache_entry(fixed_portion->name_len, NULL);
	pc_item->ce->ce_namelen = fixed_portion->name_len;
	pc_item->ce->ce_mode = fixed_portion->ce_mode;
	memcpy(pc_item->ce->name, variant, pc_item->ce->ce_namelen);
	oidcpy(&pc_item->ce->oid, &fixed_portion->oid);

	pc_item->id = fixed_portion->id;
	pc_item->ca.crlf_action = fixed_portion->crlf_action;
	pc_item->ca.ident = fixed_portion->ident;
	pc_item->ca.working_tree_encoding = encoding;
}

static void report_result(struct parallel_checkout_item *pc_item)
{
	struct pc_item_result res = { 0 };
	size_t size;

	res.id = pc_item->id;
	res.status = pc_item->status;

	if (pc_item->status == PC_ITEM_WRITTEN) {
		res.st = pc_item->st;
		size = sizeof(res);
	} else {
		size = PC_ITEM_RESULT_BASE_SIZE;
	}

	packet_write(1, (const char *)&res, size);
}

/* Free the worker-side malloced data, but not pc_item itself. */
static void release_pc_item_data(struct parallel_checkout_item *pc_item)
{
	free((char *)pc_item->ca.working_tree_encoding);
	discard_cache_entry(pc_item->ce);
}

static void worker_loop(struct checkout *state)
{
	struct parallel_checkout_item *items = NULL;
	size_t i, nr = 0, alloc = 0;

	while (1) {
		int len;
		char *line = packet_read_line(0, &len);

		if (!line)
			break;

		ALLOC_GROW(items, nr + 1, alloc);
		packet_to_pc_item(line, len, &items[nr++]);
	}

	for (i = 0; i < nr; i++) {
		struct parallel_checkout_item *pc_item = &items[i];
		write_pc_item(pc_item, state);
		report_result(pc_item);
		release_pc_item_data(pc_item);
	}

	packet_flush(1);

	free(items);
}

static const char * const checkout_helper_usage[] = {
	N_("git checkout--helper [<options>]"),
	NULL
};

int cmd_checkout__helper(int argc, const char **argv, const char *prefix)
{
	struct checkout state = CHECKOUT_INIT;
	struct option checkout_helper_options[] = {
		OPT_STRING(0, "prefix", &state.base_dir, N_("string"),
			N_("when creating files, prepend <string>")),
		OPT_END()
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage_with_options(checkout_helper_usage,
				   checkout_helper_options);

	git_config(git_default_config, NULL);
	argc = parse_options(argc, argv, prefix, checkout_helper_options,
			     checkout_helper_usage, 0);
	if (argc > 0)
		usage_with_options(checkout_helper_usage, checkout_helper_options);

	if (state.base_dir)
		state.base_dir_len = strlen(state.base_dir);

	/*
	 * Setting this on worker won't actually update the index. We just need
	 * to pretend so to induce the checkout machinery to stat() the written
	 * entries.
	 */
	state.refresh_cache = 1;

	worker_loop(&state);
	return 0;
}
