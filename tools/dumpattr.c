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
  LongNV *nv = s->attrs[entry];
  if (nv)
    {
      while (nv->name)
        {
	  fprintf (stdout, "%s:", id2str (s->pool, s->nameids[nv->name]));
	  switch (nv->type)
	    {
	      case ATTR_INT:
	        fprintf (stdout, "int  %u\n", nv->v.i[0]);
		break;
	      case ATTR_CHUNK:
	        fprintf (stdout, "blob %u+%u\n", nv->v.i[0], nv->v.i[1]);
		break;
	      case ATTR_STRING:
	        fprintf (stdout, "str  %s\n", nv->v.str);
		break;
	      case ATTR_ID:
	        fprintf (stdout, "id   %u\n", nv->v.i[0]);
		break;
	      case ATTR_INTLIST:
	        {
		  unsigned i = 0;
		  int val;
	          fprintf (stdout, "lint\n ");
		  while ((val = nv->v.intlist[i++]))
		    fprintf (stdout, " %d", val);
		  fprintf (stdout, "\n");
		  break;
		}
	      case ATTR_LOCALIDS:
	        {
		  unsigned i = 0;
		  LocalId id;
	          fprintf (stdout, "lids");
		  while ((id = nv->v.localids[i++]))
		    fprintf (stdout, "\n  %s(%d)", localid2str (s, id), id);
		  fprintf (stdout, "\n");
		  break;
		}
	      default:
	        break;
	    }
	  nv++;
	}
    }
}

int
main (void)
{
  unsigned int i;
  Pool *pool = pool_create ();
  Attrstore *s = attr_store_read (stdin, pool);
  fprintf (stdout, "attribute store contains %d entities\n", s->entries);
  for (i = 0; i < s->entries; i++)
    {
      fprintf (stdout, "\nentity %u:\n", i);
      dump_attrs (s, i);
    }
  return 0;
}
