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

#define EVRCMP_COMPARE			0
#define EVRCMP_MATCH_RELEASE		1
#define EVRCMP_MATCH			2
#define EVRCMP_COMPARE_EVONLY		3

extern int sat_vercmp(const char *s1, const char *q1, const char *s2, const char *q2);

extern int pool_evrcmp_str(const Pool *pool, const char *evr1, const char *evr2, int mode);
extern int pool_evrcmp(const Pool *pool, Id evr1id, Id evr2id, int mode);
extern int pool_evrmatch(const Pool *pool, Id evrid, const char *epoch, const char *version, const char *release);

/* obsolete, do not use in new code */
static inline int vercmp(const char *s1, const char *q1, const char *s2, const char *q2)
{
  return sat_vercmp(s1, q1, s2, q2);
}
static inline int evrcmp_str(const Pool *pool, const char *evr1, const char *evr2, int mode)
{
  return pool_evrcmp_str(pool, evr1, evr2, mode);
}
static inline int evrcmp(const Pool *pool, Id evr1id, Id evr2id, int mode)
{
  return pool_evrcmp(pool, evr1id, evr2id, mode);
}
static inline int evrmatch(const Pool *pool, Id evrid, const char *epoch, const char *version, const char *release)
{
  return pool_evrmatch(pool, evrid, epoch, version, release);
}

#ifdef __cplusplus
}
#endif

#endif /* SATSOLVER_EVR_H */
