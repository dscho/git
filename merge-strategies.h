#ifndef MERGE_STRATEGIES_H
#define MERGE_STRATEGIES_H

#include "commit.h"
#include "object.h"

int merge_strategies_one_file(struct repository *r,
			      const struct object_id *orig_blob,
			      const struct object_id *our_blob,
			      const struct object_id *their_blob, const char *path,
			      unsigned int orig_mode, unsigned int our_mode,
			      unsigned int their_mode);

typedef int (*merge_cb)(const struct object_id *orig_blob,
			const struct object_id *our_blob,
			const struct object_id *their_blob, const char *path,
			unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
			void *data);

int merge_one_file_cb(const struct object_id *orig_blob,
		      const struct object_id *our_blob,
		      const struct object_id *their_blob, const char *path,
		      unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
		      void *data);

int merge_program_cb(const struct object_id *orig_blob,
		     const struct object_id *our_blob,
		     const struct object_id *their_blob, const char *path,
		     unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
		     void *data);

int merge_one_path(struct index_state *istate, int oneshot, int quiet,
		   const char *path, merge_cb cb, void *data);
int merge_all(struct index_state *istate, int oneshot, int quiet,
	      merge_cb cb, void *data);

int merge_strategies_resolve(struct repository *r,
			     struct commit_list *bases, const char *head_arg,
			     struct commit_list *remote);
int merge_strategies_octopus(struct repository *r,
			     struct commit_list *bases, const char *head_arg,
			     struct commit_list *remote);

#endif /* MERGE_STRATEGIES_H */
