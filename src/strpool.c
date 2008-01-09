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

void
stringpool_init (Stringpool *ss, const char *strs[])
{
  unsigned totalsize = 0;
  unsigned count;
  // count number and total size of predefined strings
  for (count = 0; strs[count]; count++)
    totalsize += strlen(strs[count]) + 1;

  // alloc appropriate space
  ss->stringspace = sat_malloc((totalsize + STRINGSPACE_BLOCK) & ~STRINGSPACE_BLOCK);
  ss->strings = sat_malloc2((count + STRING_BLOCK) & ~STRING_BLOCK, sizeof(Offset));

  // now copy predefined strings into allocated space
  ss->sstrings = 0;
  for (count = 0; strs[count]; count++)
    {
      strcpy(ss->stringspace + ss->sstrings, strs[count]);
      ss->strings[count] = ss->sstrings;
      ss->sstrings += strlen(strs[count]) + 1;
    }
  ss->nstrings = count;
}

Id
stringpool_strn2id (Stringpool *ss, const char *str, unsigned len, int create)
{
  Hashval h;
  unsigned int hh;
  Hashmask hashmask;
  int i, space_needed;
  Id id;
  Hashtable hashtbl;

  // check string
  if (!str)
    return STRID_NULL;
  if (!*str)
    return STRID_EMPTY;

  hashmask = ss->stringhashmask;
  hashtbl = ss->stringhashtbl;

  // expand hashtable if needed
  // 
  // 
  if (ss->nstrings * 2 > hashmask)
    {
      sat_free(hashtbl);

      // realloc hash table
      ss->stringhashmask = hashmask = mkmask(ss->nstrings + STRING_BLOCK);
      ss->stringhashtbl = hashtbl = (Hashtable)sat_calloc(hashmask + 1, sizeof(Id));

      // rehash all strings into new hashtable
      for (i = 1; i < ss->nstrings; i++)
	{
	  h = strhash(ss->stringspace + ss->strings[i]) & hashmask;
	  hh = HASHCHAIN_START;
	  while (hashtbl[h] != 0)  // follow overflow chain
	    h = HASHCHAIN_NEXT(h, hh, hashmask);
	  hashtbl[h] = i;
	}
    }

  // compute hash and check for match

  h = strnhash(str, len) & hashmask;
  hh = HASHCHAIN_START;
  while ((id = hashtbl[h]) != 0)  // follow hash overflow chain
    {
      // break if string already hashed
      if(!memcmp(ss->stringspace + ss->strings[id], str, len)
         && ss->stringspace[ss->strings[id] + len] == 0)
	break;
      h = HASHCHAIN_NEXT(h, hh, hashmask);
    }
  if (id || !create)    // exit here if string found
    return id;

  // generate next id and save in table
  id = ss->nstrings++;
  hashtbl[h] = id;

  // 
  if ((id & STRING_BLOCK) == 0)
    ss->strings = sat_realloc2(ss->strings, (ss->nstrings + STRING_BLOCK) & ~STRING_BLOCK, sizeof(Hashval));
  // 'pointer' into stringspace is Offset of next free pos: sstrings
  ss->strings[id] = ss->sstrings;

  space_needed = len + 1;

  // resize string buffer if needed
  if (((ss->sstrings + space_needed - 1) | STRINGSPACE_BLOCK) != ((ss->sstrings - 1) | STRINGSPACE_BLOCK))
    ss->stringspace = sat_realloc(ss->stringspace, (ss->sstrings + space_needed + STRINGSPACE_BLOCK) & ~STRINGSPACE_BLOCK);
  // copy new string into buffer
  memcpy(ss->stringspace + ss->sstrings, str, space_needed - 1);
  // add the sentinel, we can't rely on it being in the source string (in
  // case the LEN is not really strlen(str))
  ss->stringspace[ss->sstrings + space_needed - 1] = 0;
  // next free pos is behind new string
  ss->sstrings += space_needed;

  return id;
}

Id
stringpool_str2id (Stringpool *ss, const char *str, int create)
{
  unsigned len = strlen (str);
  return stringpool_strn2id (ss, str, len, create);
}

void
stringpool_shrink (Stringpool *ss)
{
  ss->stringspace = (char *)sat_realloc(ss->stringspace, (ss->sstrings + STRINGSPACE_BLOCK) & ~STRINGSPACE_BLOCK);
  ss->strings = (Offset *)sat_realloc2(ss->strings, (ss->nstrings + STRING_BLOCK) & ~STRING_BLOCK, sizeof(Offset));
}
