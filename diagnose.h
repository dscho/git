#ifndef DIAGNOSE_H
#define DIAGNOSE_H

#include "cache.h"
#include "strbuf.h"

int create_diagnostics_archive(struct strbuf *zip_path, int include_everything);

#endif /* DIAGNOSE_H */
