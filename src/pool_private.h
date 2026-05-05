/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pool_private.h
 *
 */

#ifndef LIBSOLV_POOL_PRIVATE_H
#define LIBSOLV_POOL_PRIVATE_H

/* the size of all buffers is incremented in blocks */
#define WHATPROVIDES_BLOCK	1023

Id *pool_lookup_languagecache_row(Pool *pool, Id keyname);

#endif /* LIBSOLV_POOL_PRIVATE_H */
