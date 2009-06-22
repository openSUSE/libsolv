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

#ifndef SATSOLVER_EVR_H
#define SATSOLVER_EVR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pooltypes.h"

#define EVRCMP_COMPARE                  0
#define EVRCMP_MATCH_RELEASE            1
#define EVRCMP_MATCH                    2

extern int vercmp(const char *s1, const char *q1, const char *s2, const char *q2);
extern int evrcmp_str(const Pool *pool, const char *evr1, const char *evr2, int mode);
extern int evrcmp(const Pool *pool, Id evr1id, Id evr2id, int mode);
extern int evrmatch(const Pool *pool, Id evrid, const char *epoch, const char *version, const char *release);

#ifdef __cplusplus
}
#endif

#endif /* SATSOLVER_EVR_H */
