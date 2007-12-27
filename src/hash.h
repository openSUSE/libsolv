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

#ifndef SATSOLVER_HASH_H
#define SATSOLVER_HASH_H

#include "pooltypes.h"

/* value of a hash */
typedef unsigned int Hashval;
/* mask for hash, used as modulo operator to ensure 'wrapping' of hash
   values -> hash table */
typedef unsigned int Hashmask;

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

/* hash for rel
 * rel -> hash
 */
static inline Hashval
relhash(Id name, Id evr, int flags)
{
  return name + 7 * evr + 13 * flags;
}


/* compute bitmask for value
 * returns smallest (2^n-1) > num
 * 
 * used for Hashtable 'modulo' operation
 */ 
static inline Hashmask
mkmask(unsigned int num)
{
  num *= 2;
  while (num & (num - 1))
    num &= num - 1;
  return num * 2 - 1;
}

#endif /* SATSOLVER_HASH_H */
