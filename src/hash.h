/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * hash.h
 * generic hash functions
 */

#ifndef LIBSOLV_HASH_H
#define LIBSOLV_HASH_H

#include "pooltypes.h"
#include "util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* value of a hash */
typedef unsigned int Hashval;

/* inside the hash table, Ids are stored. Hash maps: string -> hash -> Id */
typedef Id *Hashtable;

/* hash chain */
#define HASHCHAIN_START 7
#define HASHCHAIN_NEXT(h, hh, mask) (((h) + (hh)++) & (mask))

/* very simple hash function
 * string -> hash
 */
static inline Hashval
strhash(const char *str)
{
  Hashval r = 0;
  unsigned int c;
  while ((c = *(const unsigned char *)str++) != 0)
    r += (r << 3) + c;
  return r;
}

static inline Hashval
strnhash(const char *str, unsigned len)
{
  Hashval r = 0;
  unsigned int c;
  while (len-- && (c = *(const unsigned char *)str++) != 0)
    r += (r << 3) + c;
  return r;
}

static inline Hashval
strhash_cont(const char *str, Hashval r)
{
  unsigned int c;
  while ((c = *(const unsigned char *)str++) != 0)
    r += (r << 3) + c;
  return r;
}


/* hash for rel
 * rel -> hash
 */
static inline Hashval
relhash(Id name, Id evr, int flags)
{
  return name + 7 * evr + 13 * flags;
}


/* compute bitmask for value
 * returns smallest (2^n-1) > 2 * num + 3
 *
 * used for Hashtable 'modulo' operation
 */
static inline Hashval
mkmask(unsigned int num)
{
  if (num >= 0x3ffffffe)
    return num >= 0x80000000 ? 0 : 0xffffffff;
  num = num * 2 + 3;
  while (num & (num - 1))
    num &= num - 1;
  return num * 2 - 1;
}

static inline Hashtable
allochashtable(Hashval mask, size_t size)
{
  if (mask == 0 && ((size_t)mask + 1) == 0)
    solv_oom((size_t)mask, size * sizeof(Id));
  return (Hashtable)solv_calloc((size_t)mask + 1, size * sizeof(Id));
}

#ifdef __cplusplus
}
#endif

#endif /* LIBSOLV_HASH_H */
