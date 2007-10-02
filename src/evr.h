/*
 * evr.h
 * 
 */

#ifndef EVR_H
#define EVR_H

#include "pooltypes.h"

extern int vercmp( const char *s1, const char *q1, const char *s2, const char *q2 );
extern int evrcmp( Pool *pool, Id evr1id, Id evr2id );

#endif /* EVR_H */
