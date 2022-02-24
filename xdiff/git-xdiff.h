#ifndef GIT_XDIFF_H
#define GIT_XDIFF_H

#include "git-compat-util.h"

#define xdl_malloc(x) xmalloc(x)
#define xdl_free(ptr) free(ptr)
#define xdl_realloc(ptr,x) xrealloc(ptr,x)

#define xdl_regex_t regex_t
#define xdl_regmatch_t regmatch_t
#define xdl_regexec_buf(p, b, s, n, m, f) regexec_buf(p, b, s, n, m, f)

#define XDL_BUG(msg) BUG(msg)

#endif
