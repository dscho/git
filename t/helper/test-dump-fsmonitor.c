#include "test-tool.h"
#include "cache.h"

int cmd__dump_fsmonitor(int ac, const char **av)
{
	struct index_state *istate = the_repository->index;
	int i, valid = 0;

	setup_git_directory();
	if (do_read_index(istate, the_repository->index_file, 0) < 0)
		die("unable to read index file");
	if (!istate->fsmonitor_last_update) {
		printf("no fsmonitor\n");
		return 0;
	}
	printf("fsmonitor last update %s\n", istate->fsmonitor_last_update);

	for (i = 0; i < istate->cache_nr; i++) {
		printf((istate->cache[i]->ce_flags & CE_FSMONITOR_VALID) ? "+" : "-");
		if (istate->cache[i]->ce_flags & CE_FSMONITOR_VALID)
			valid++;
	}

	printf("\n  valid:   %d\n", valid);
	printf("  invalid: %d\n", istate->cache_nr - valid);

	for (i = 0; i < istate->cache_nr; i++)
		if (!(istate->cache[i]->ce_flags & CE_FSMONITOR_VALID))
			printf("   - %s\n", istate->cache[i]->name);

	return 0;
}
