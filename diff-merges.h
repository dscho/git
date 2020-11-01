#ifndef DIFF_MERGES_H
#define DIFF_MERGES_H

struct rev_info;

void init_diff_merge_revs(struct rev_info *revs);
int parse_diff_merge_opts(struct rev_info *revs, const char **argv);
void setup_diff_merges_revs(struct rev_info *revs);
void rev_diff_merges_default_to_dense_combined(struct rev_info *revs);
void rev_diff_merges_first_parent_defaults_to_enable(struct rev_info *revs);

#endif
