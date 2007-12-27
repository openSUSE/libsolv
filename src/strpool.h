/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */
#ifndef SATSOLVER_STRINGPOOL_H
#define SATSOLVER_STRINGPOOL_H

#include "pooltypes.h"
#include "hash.h"

#define STRID_NULL  0
#define STRID_EMPTY 1

struct _Stringpool
{
  Offset *strings;            // table of offsets into stringspace, indexed by Id: Id -> Offset
  int nstrings;               // number of unique strings in stringspace
  char *stringspace;          // space for all unique strings: stringspace + Offset = string
  Offset sstrings;            // next free pos in stringspace

  Hashtable stringhashtbl;    // hash table: (string ->) Hash -> Id
  Hashmask stringhashmask;    // modulo value for hash table (size of table - 1)
};

void stringpool_init (Stringpool *ss, const char *strs[]);
Id stringpool_str2id (Stringpool *ss, const char *str, int create);
Id stringpool_strn2id (Stringpool *ss, const char *str, unsigned len, int create);
void stringpool_shrink (Stringpool *ss);

static inline const char *
stringpool_id2str (Stringpool *ss, Id id)
{
  return ss->stringspace + ss->strings[id];
}

#endif
