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

#ifndef POOLTYPES_H
#define POOLTYPES_H

/* version number for .solv files */
#define SOLV_VERSION 0

struct _Pool;
typedef struct _Pool Pool;

// identifier for string values
typedef int Id;		/* must be signed!, since negative Id is used in solver rules to denote negation */

// offset value, e.g. used to 'point' into the stringspace
typedef unsigned int Offset;

#endif /* POOLTYPES_H */
