/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <string.h>
#include "util.h"
#include "strpool.h"

#define STRING_BLOCK      2047
#define STRINGSPACE_BLOCK 65535

#define STRING_MAXSIZE 0x40000000

void
stringpool_init(Stringpool *ss, const char *strs[])
{
  unsigned totalsize = 0;
  unsigned count;

  memset(ss, 0, sizeof(*ss));
  /* count number and total size of predefined strings */
  for (count = 0; strs[count]; count++)
    totalsize += strlen(strs[count]) + 1;

  /* alloc appropriate space */
  ss->stringspace = solv_extend_resize(0, totalsize, 1, STRINGSPACE_BLOCK);
  ss->strings = solv_extend_resize(0, count, sizeof(Offset), STRING_BLOCK);

  /* now copy predefined strings into allocated space */
  ss->sstrings = 0;
  for (count = 0; strs[count]; count++)
    {
      strcpy(ss->stringspace + ss->sstrings, strs[count]);
      ss->strings[count] = ss->sstrings;
      ss->sstrings += strlen(strs[count]) + 1;
    }
  ss->nstrings = count;
}

void
stringpool_free(Stringpool *ss)
{
  solv_free(ss->strings);
  solv_free(ss->stringspace);
  solv_free(ss->stringhashtbl);
}

void
stringpool_freehash(Stringpool *ss)
{
  ss->stringhashtbl = solv_free(ss->stringhashtbl);
  ss->stringhashmask = 0;
}

void
stringpool_init_empty(Stringpool *ss)
{
  static const char *emptystrs[] = { "<NULL>", "", 0 };
  stringpool_init(ss, emptystrs);
}

void
stringpool_clone(Stringpool *ss, Stringpool *from)
{
  memset(ss, 0, sizeof(*ss));
  ss->strings = solv_extend_resize(0, from->nstrings, sizeof(Offset), STRING_BLOCK);
  memcpy(ss->strings, from->strings, from->nstrings * sizeof(Offset));
  ss->stringspace = solv_extend_resize(0, from->sstrings, 1, STRINGSPACE_BLOCK);
  memcpy(ss->stringspace, from->stringspace, from->sstrings);
  ss->nstrings = from->nstrings;
  ss->sstrings = from->sstrings;
}

void
stringpool_resize_hash(Stringpool *ss, int numnew)
{
  Hashval h, hh, hashmask;
  Hashtable hashtbl;
  int i;

  if (numnew <= 0)
    return;
  hashmask = mkmask(ss->nstrings + numnew);
  if (hashmask <= ss->stringhashmask)
    return;	/* same as before */

  /* realloc hash table */
  ss->stringhashmask = hashmask;
  solv_free(ss->stringhashtbl);
  ss->stringhashtbl = hashtbl = allochashtable(hashmask, 1);
  
  /* rehash all strings into new hashtable */
  for (i = 1; i < ss->nstrings; i++)
    {
      h = strhash(ss->stringspace + ss->strings[i]) & hashmask;
      hh = HASHCHAIN_START;
      while (hashtbl[h] != 0)
	h = HASHCHAIN_NEXT(h, hh, hashmask);
      hashtbl[h] = i;
    }
}

Id
stringpool_strn2id(Stringpool *ss, const char *str, unsigned int len, int create)
{
  Hashval h, hh, hashmask;
  Id id;
  Hashtable hashtbl;

  if (!str)
    return STRID_NULL;
  if (!len)
    return STRID_EMPTY;

  if (len >= STRING_MAXSIZE)
    {
      solv_ovfl("maximum string size overflow");
      return 0;
    }

  hashmask = ss->stringhashmask;

  /* expand hashtable if needed */
  if ((Hashval)ss->nstrings * 2 >= hashmask)
    {
      /* this should be a test for a flag that tells us if the
       * correct blocking is used, but adding a flag would break
       * the ABI. So we use the existance of the hash area as
       * indication instead */
      if (!hashmask)
	stringpool_reserve(ss, 1, len + 1);
      stringpool_resize_hash(ss, STRING_BLOCK);
      hashmask = ss->stringhashmask;
    }
  hashtbl = ss->stringhashtbl;

  /* compute hash and check for match */
  h = strnhash(str, len) & hashmask;
  hh = HASHCHAIN_START;
  while ((id = hashtbl[h]) != 0)
    {
      if(!memcmp(ss->stringspace + ss->strings[id], str, len)
         && ss->stringspace[ss->strings[id] + len] == 0)
	break;
      h = HASHCHAIN_NEXT(h, hh, hashmask);
    }
  if (id || !create)    /* exit here if string found */
    return id;

  /* generate next id and save in table */
  id = ss->nstrings++;
  hashtbl[h] = id;

  ss->strings = solv_extend(ss->strings, id, 1, sizeof(Offset), STRING_BLOCK);
  ss->strings[id] = ss->sstrings;	/* we will append to the end */

  /* append string to stringspace */
  ss->stringspace = solv_extend(ss->stringspace, ss->sstrings, len + 1, 1, STRINGSPACE_BLOCK);
  memcpy(ss->stringspace + ss->sstrings, str, len);
  ss->stringspace[ss->sstrings + len] = 0;
  ss->sstrings += len + 1;
  return id;
}

Id
stringpool_str2id(Stringpool *ss, const char *str, int create)
{
  size_t len;
  if (!str)
    return STRID_NULL;
  if (!*str)
    return STRID_EMPTY;
  len = strlen(str);
  if (len >= STRING_MAXSIZE)
    {
      solv_ovfl("maximum string size overflow");
      return 0;
    }
  return stringpool_strn2id(ss, str, (unsigned int)len, create);
}

void
stringpool_shrink(Stringpool *ss)
{
  ss->stringspace = solv_extend_resize(ss->stringspace, ss->sstrings, 1, STRINGSPACE_BLOCK);
  ss->strings = solv_extend_resize(ss->strings, ss->nstrings, sizeof(Offset), STRING_BLOCK);
}

void
stringpool_reserve(Stringpool *ss, int numid, Offset sizeid)
{
  ss->stringspace = solv_extend_resize(ss->stringspace, ss->sstrings + sizeid, 1, STRINGSPACE_BLOCK);
  ss->strings = solv_extend_resize(ss->strings, ss->nstrings + numid, sizeof(Offset), STRING_BLOCK);
}

/* this seems to be the sweet spot */
#define INTEGRATE_SPOOL_CHUNK 16

static char *
integrate_chunk(Stringpool *ss, int numid, char *sp, char *endsp, Id *idmap)
{
  unsigned int l, len[INTEGRATE_SPOOL_CHUNK];
  Hashval hs[INTEGRATE_SPOOL_CHUNK];
  Hashval h, hh;
  Hashtable hashtbl = ss->stringhashtbl;
  Hashval hashmask = ss->stringhashmask;
  Id id;
  char *spstart = sp;
  Offset *str = ss->strings;
  int i;

  if (numid > INTEGRATE_SPOOL_CHUNK)
    return 0;
  for (i = 0; i < numid; i++)
    {
      if (sp >= endsp)
	return 0;
      if (!*sp)
	{
	  len[i] = 0;
	  hs[i] = 0;
	  sp++;
	  continue;
	}
      l = (unsigned int)strlen(sp);
      h = strnhash(sp, l) & hashmask;
      solv_prefetch(hashtbl + h);
      len[i] = l;
      hs[i] = h;
      sp += l + 1;
    }
  sp = spstart;
  for (i = 0; i < numid; i++)
    {
      l = len[i];
      if (!l)
	{
	  idmap[i] = STRID_EMPTY;
	  sp++;
	  continue;
	}
      l++;
      h = hs[i];
      hh = HASHCHAIN_START;
      for (;;)
	{
	  id = hashtbl[h];
	  if (!id)
	    break;
	  if (!strcmp(ss->stringspace + ss->strings[id], sp))
	    break;			/* already in pool */
	  h = HASHCHAIN_NEXT(h, hh, hashmask);
	}
      if (!id)				/* end of hash chain -> new string */
	{
	  id = ss->nstrings++;
	  hashtbl[h] = id;
	  str[id] = ss->sstrings;	/* save offset */
	  if (sp != ss->stringspace + ss->sstrings)
	    memmove(ss->stringspace + ss->sstrings, sp, l);
	  ss->sstrings += l;
	}
      idmap[i] = id;			/* repo relative -> pool relative */
      sp += l;				/* next string */
    }
  return sp;
}

int
stringpool_integrate(Stringpool *ss, int numid, Offset sizeid, Id *idmap)
{
  char *sp = ss->stringspace + ss->sstrings;
  char *endsp = sp + sizeid;
  int i;

  stringpool_resize_hash(ss, numid);
  for (i = 1; i < numid;)
    {
      int chunk = numid - i > INTEGRATE_SPOOL_CHUNK ? INTEGRATE_SPOOL_CHUNK : numid - i;
      sp = integrate_chunk(ss, chunk, sp, endsp, idmap + i);
      if (!sp)
	return 0;
      i += chunk;
    }
  if ((unsigned int)ss->nstrings >= (unsigned int)SOLV_MAX_INDEX)
    solv_ovfl("string count overpool");
  return 1;
}

