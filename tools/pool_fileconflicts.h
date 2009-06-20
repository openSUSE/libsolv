#ifndef POOL_FILECONFLICTS_H
#define POOL_FILECONFLICTS_H

#include "pool.h"

extern int pool_findfileconflicts(Pool *pool, Queue *pkgs, Queue *conflicts, void *(*handle_cb)(Pool *, Id, void *) , void *handle_cbdata);

#endif
