/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * evr.h
 * 
 */

#ifndef EVR_H
#define EVR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pooltypes.h"

extern int vercmp( const char *s1, const char *q1, const char *s2, const char *q2 );
extern int evrcmp( Pool *pool, Id evr1id, Id evr2id );

#ifdef __cplusplus
}
#endif

#endif /* EVR_H */
