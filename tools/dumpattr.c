#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "attr_store.h"
#include "pool.h"
#include "attr_store_p.h"

static void
dump_attrs (Attrstore *s, unsigned int entry)
{
  attr_iterator ai;
  FOR_ATTRS (s, entry, &ai)
    {
      fprintf (stdout, "%s:", id2str (s->pool, s->nameids[ai.name]));
      switch (ai.type)
	{
	case ATTR_INT:
	  fprintf (stdout, "int  %u\n", ai.as_int);
	  break;
	case ATTR_ID:
	  fprintf (stdout, "id   %u\n", ai.as_id);
	  break;
	case ATTR_CHUNK:
	  fprintf (stdout, "blob %u+%u\n", ai.as_chunk[0], ai.as_chunk[1]);
	  break;
	case ATTR_STRING:
	  fprintf (stdout, "str  %s\n", ai.as_string);
	  break;
	case ATTR_INTLIST:
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
	case ATTR_LOCALIDS:
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

int
main (void)
{
  unsigned int i;
  Pool *pool = pool_create ();
  Attrstore *s = attr_store_read (stdin, pool);
  /* For now test the packing code.  */
  attr_store_unpack (s);
  attr_store_pack (s);
  fprintf (stdout, "attribute store contains %d entities\n", s->entries);
  for (i = 0; i < s->entries; i++)
    {
      fprintf (stdout, "\nentity %u:\n", i);
      dump_attrs (s, i);
    }
  return 0;
}
