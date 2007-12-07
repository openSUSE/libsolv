/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "attr_store.h"
#include "pool.h"
#include "attr_store_p.h"

static void
dump_attrs (Attrstore *s, unsigned int entry)
{
  attr_iterator ai;
  FOR_ATTRS (s, entry, &ai)
    {
      fprintf (stdout, "%s:", id2str (s->pool, ai.name));
      switch (ai.type)
	{
	case TYPE_ATTR_INT:
	  fprintf (stdout, "int  %u\n", ai.as_int);
	  break;
	case TYPE_ATTR_CHUNK:
	  {
	    const char *str = attr_retrieve_blob (s, ai.as_chunk[0], ai.as_chunk[1]);
	    if (str)
	      fprintf (stdout, "blob %s\n", str);
	    else
	      fprintf (stdout, "blob %u+%u\n", ai.as_chunk[0], ai.as_chunk[1]);
	  }
	  break;
	case TYPE_ATTR_STRING:
	  fprintf (stdout, "str  %s\n", ai.as_string);
	  break;
	case TYPE_ATTR_INTLIST:
	  {
	    fprintf (stdout, "lint\n ");
	    while (1)
	      {
		int val;
		get_num (ai.as_numlist, val);
		if (!val)
		  break;
		fprintf (stdout, " %d", val);
	      }
	    fprintf (stdout, "\n");
	    break;
	  }
	case TYPE_ATTR_LOCALIDS:
	  {
	    fprintf (stdout, "lids");
	    while (1)
	      {
		Id val;
		get_num (ai.as_numlist, val);
		if (!val)
		  break;
		fprintf (stdout, "\n  %s(%d)", localid2str (s, val), val);
	      }
	    fprintf (stdout, "\n");
	    break;
	  }
	default:
	  break;
	}
    }
}

static int
callback (Attrstore *s, unsigned entry, Id name, const char *str)
{
  fprintf (stdout, "%d:%s:%s\n", entry, id2str (s->pool, name), str);
  return 1;
}

int
main (int argc, char *argv[])
{
  unsigned int i;
  Pool *pool = pool_create ();
  Attrstore *s = attr_store_read (stdin, pool);
  /* For now test the packing code.  */
  attr_store_unpack (s);
  attr_store_pack (s);
  fprintf (stdout, "attribute store contains %d entities\n", s->entries);
  if (argc == 1)
    for (i = 0; i < s->entries; i++)
      {
        fprintf (stdout, "\nentity %u:\n", i);
        dump_attrs (s, i);
      }
  else
    {
      int g;
      unsigned flags;
      unsigned search_type;
      Id name;
      name = 0;
      flags = SEARCH_IDS;
      search_type = SEARCH_SUBSTRING;
      while ((g = getopt (argc, argv, "-n:bgeri")) >= 0)
        switch (g)
	{
	  case 'n': name = str2id (s->pool, optarg, 1); break;
	  case 'b': flags |= SEARCH_BLOBS; break;
	  case 'g': search_type = SEARCH_GLOB; break;
	  case 'e': search_type = SEARCH_STRING; break;
	  case 'r': search_type = SEARCH_REGEX; break;
	  case 'i': flags |= SEARCH_NOCASE; break;
	  case 1:
	    attr_store_search_s (s, optarg, search_type | flags, name, callback);
	    flags = SEARCH_IDS;
	    name = 0;
	    search_type = SEARCH_SUBSTRING;
	    break;
	}
    }
  return 0;
}
