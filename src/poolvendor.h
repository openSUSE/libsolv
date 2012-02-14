/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef LIBSOLV_POOLVENDOR_H
#define LIBSOLV_POOLVENDOR_H

#include "pool.h"

Id pool_vendor2mask(Pool *pool, Id vendor);
void pool_setvendorclasses(Pool *pool, const char **vendorclasses);
void pool_addvendorclass(Pool *pool, const char **vendorclass);

#endif /* LIBSOLV_POOLVENDOR_H */
