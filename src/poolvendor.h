/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef SATSOLVER_POOLVENDOR_H
#define SATSOLVER_POOLVENDOR_H

#include "pool.h"

Id pool_vendor2mask(Pool *pool, Id vendor);
void pool_setvendorclasses(Pool *pool, const char **vendorclasses);

#endif /* SATSOLVER_POOLVENDOR_H */
