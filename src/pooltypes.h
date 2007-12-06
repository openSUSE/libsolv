/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * pooltypes.h
 * 
 */

#ifndef SATSOLVER_POOLTYPES_H
#define SATSOLVER_POOLTYPES_H

/* version number for .solv files */
#define SOLV_VERSION_0 0
#define SOLV_VERSION_1 1
#define SOLV_VERSION_2 2
#define SOLV_FLAG_PACKEDSIZES 1
#define SOLV_FLAG_VERTICAL    2
#define SOLV_FLAG_PREFIX_POOL 4

struct _Stringpool;
typedef struct _Stringpool Stringpool;

struct _Pool;
typedef struct _Pool Pool;

// identifier for string values
typedef int Id;		/* must be signed!, since negative Id is used in solver rules to denote negation */

// offset value, e.g. used to 'point' into the stringspace
typedef unsigned int Offset;

#endif /* SATSOLVER_POOLTYPES_H */
