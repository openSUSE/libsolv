/*
 * source_write.h
 *
 */

#ifndef SOURCE_WRITE_H
#define SOURCE_WRITE_H

#include <stdio.h>

#include "pool.h"
#include "source.h"

extern void pool_writesource(Pool *pool, Source *source, FILE *fp);

#endif
