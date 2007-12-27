/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* We need FNM_CASEFOLD and strcasestr.  */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fnmatch.h>
#include <regex.h>

#include "attr_store.h"
#include "pool.h"
#include "repo.h"
#include "util.h"

#include "attr_store_p.h"

#include "fastlz.c"

/* #define DEBUG_PAGING */

#define BLOB_BLOCK 65535

#define STRINGSPACE_BLOCK 1023
#define STRING_BLOCK 127
#define LOCALID_NULL  0
#define LOCALID_EMPTY 1

static Id add_key (Attrstore *s, Id name, unsigned type, unsigned size);

Attrstore *
new_store (Pool *pool)
{
  static const char *predef_strings[] = {
    "<NULL>",
    "",
    0
  };
  Attrstore *s = calloc (1, sizeof (Attrstore));
  s->pool = pool;
  stringpool_init (&s->ss, predef_strings);
  add_key (s, 0, 0, 0);

  return s;
}

LocalId
str2localid (Attrstore *s, const char *str, int create)
{
  return stringpool_str2id (&s->ss, str, create);
}

const char *
localid2str(Attrstore *s, LocalId id)
{
  return s->ss.stringspace + s->ss.strings[id];
}

static void
setup_dirs (Attrstore *s)
{
  static const char *ss_init_strs[] =
  {
    "<NULL>",
    "",
    0
  };

  s->dirtree.dirs = calloc (1024, sizeof (s->dirtree.dirs[0]));
  s->dirtree.ndirs = 2;
  s->dirtree.dirs[0].child = 0;
  s->dirtree.dirs[0].sibling = 0;
  s->dirtree.dirs[0].name = 0;
  s->dirtree.dirs[1].child = 0;
  s->dirtree.dirs[1].sibling = 0;
  s->dirtree.dirs[1].name = STRID_EMPTY;

  s->dirtree.dirstack_size = 16;
  s->dirtree.dirstack = malloc (s->dirtree.dirstack_size * sizeof (s->dirtree.dirstack[0]));
  s->dirtree.ndirstack = 0;
  s->dirtree.dirstack[s->dirtree.ndirstack++] = 1; //dir-id of /

  stringpool_init (&s->dirtree.ss, ss_init_strs);
}

static unsigned
dir_lookup_1 (Attrstore *s, unsigned dir, const char *name, unsigned insert)
{
  Id nameid;
  while (*name == '/')
    name++;
  if (!*name)
    return dir;
  const char *end = strchrnul (name, '/');
  nameid = stringpool_strn2id (&s->dirtree.ss, name, end - name, 1);
  unsigned c, num = 0;
  Dir *dirs = s->dirtree.dirs;
  for (c = dirs[dir].child; c; c = dirs[c].sibling, num++)
    if (nameid == dirs[c].name)
      break;
  if (!c && !insert)
    return 0;
  if (!c)
    {
      c = s->dirtree.ndirs++;
      if (!(c & 1023))
	dirs = realloc (dirs, (c + 1024) * sizeof (dirs[0]));
      dirs[c].child = 0;
      dirs[c].sibling = dirs[dir].child;
      dirs[c].name = nameid;
      dirs[c].parent = dir;
      dirs[dir].child = c;
      s->dirtree.dirs = dirs;
    }
  if (!(s->dirtree.ndirstack & 15))
    {
      s->dirtree.dirstack_size += 16;
      s->dirtree.dirstack = realloc (s->dirtree.dirstack, s->dirtree.dirstack_size * sizeof (s->dirtree.dirstack[0]));
    }
  s->dirtree.dirstack[s->dirtree.ndirstack++] = c;
  if (!*end)
    return c;
  unsigned ret = dir_lookup_1 (s, c, end + 1, insert);
  return ret;
}

unsigned
dir_lookup (Attrstore *s, const char *name, unsigned insert)
{
  if (!s->dirtree.ndirs)
    setup_dirs (s);

  /* Detect number of common path components.  Accept multiple // .  */
  const char *new_start;
  unsigned components;
  for (components = 1, new_start = name; components < s->dirtree.ndirstack; )
    {
      int ofs;
      const char *dirname;
      while (*new_start == '/')
	new_start++;
      dirname = stringpool_id2str (&s->dirtree.ss, s->dirtree.dirs[s->dirtree.dirstack[components]].name);
      for (ofs = 0;; ofs++)
	{
	  char n = new_start[ofs];
	  char d = dirname[ofs];
	  if (d == 0 && (n == 0 || n == '/'))
	    {
	      new_start += ofs;
	      components++;
	      if (n == 0)
		goto endmatch2;
	      break;
	    }
	  if (n != d)
	    goto endmatch;
	}
    }
endmatch:
  while (*new_start == '/')
    new_start++;
endmatch2:

  /* We have always / on the stack.  */
  //assert (ndirstack);
  //assert (ndirstack >= components);
  s->dirtree.ndirstack = components;
  unsigned ret = s->dirtree.dirstack[s->dirtree.ndirstack - 1];
  if (*new_start)
    ret = dir_lookup_1 (s, ret, new_start, insert);
  //assert (ret == dirstack[ndirstack - 1]);
  return ret;
}

unsigned
dir_parent (Attrstore *s, unsigned dir)
{
  return s->dirtree.dirs[dir].parent;
}

void
dir2str (Attrstore *s, unsigned dir, char **str, unsigned *len)
{
  unsigned l = 0;
  Id ids[s->dirtree.dirstack_size + 1];
  unsigned i, ii;
  for (i = 0; dir > 1; dir = dir_parent (s, dir), i++)
    ids[i] = s->dirtree.dirs[dir].name;
  ii = i;
  l = 1;
  for (i = 0; i < ii; i++)
    l += 1 + strlen (stringpool_id2str (&s->dirtree.ss, ids[i]));
  l++;
  if (l > *len)
    {
      *str = malloc (l);
      *len = l;
    }
  char *dest = *str;
  *dest++ = '/';
  for (i = ii; i--;)
    {
      const char *name = stringpool_id2str (&s->dirtree.ss, ids[i]);
      dest = mempcpy (dest, name, strlen (name));
      *dest++ = '/';
    }
  *dest = 0;
}

void
ensure_entry (Attrstore *s, unsigned int entry)
{
  unsigned int old_num = s->entries;
  if (entry < s->entries)
    return;
  s->entries = entry + 1;
  if (((old_num + 127) & ~127) != ((s->entries + 127) & ~127))
    {
      if (s->attrs)
        s->attrs = realloc (s->attrs, (((s->entries+127) & ~127) * sizeof (s->attrs[0])));
      else
        s->attrs = malloc (((s->entries+127) & ~127) * sizeof (s->attrs[0]));
    }
  memset (s->attrs + old_num, 0, (s->entries - old_num) * sizeof (s->attrs[0]));
}

unsigned int
new_entry (Attrstore *s)
{
  if ((s->entries & 127) == 0)
    {
      if (s->attrs)
        s->attrs = realloc (s->attrs, ((s->entries+128) * sizeof (s->attrs[0])));
      else
        s->attrs = malloc ((s->entries+128) * sizeof (s->attrs[0]));
    }
  s->attrs[s->entries++] = 0;
  return s->entries - 1;
}

static LongNV *
find_attr (Attrstore *s, unsigned int entry, Id name)
{
  LongNV *nv;
  if (entry >= s->entries)
    return 0;
  nv = s->attrs[entry];
  if (nv)
    {
      while (nv->key && s->keys[nv->key].name != name)
	nv++;
      if (nv->key)
        return nv;
    }
  return 0;
}

static void
add_attr (Attrstore *s, unsigned int entry, LongNV attr)
{
  LongNV *nv;
  unsigned int len;
  ensure_entry (s, entry);
  if (attr.key >= s->nkeys)
    return;
  nv = s->attrs[entry];
  len = 0;
  if (nv)
    {
      while (nv->key && nv->key != attr.key)
	nv++;
      if (nv->key)
        return;
      len = nv - s->attrs[entry];
    }
  len += 2;
  if (s->attrs[entry])
    s->attrs[entry] = realloc (s->attrs[entry], len * sizeof (LongNV));
  else
    s->attrs[entry] = malloc (len * sizeof (LongNV));
  nv = s->attrs[entry] + len - 2;
  *nv++ = attr;
  nv->key = 0;
}

void
add_attr_int (Attrstore *s, unsigned int entry, Id name, unsigned int val)
{
  LongNV nv;
  nv.key = add_key (s, name, TYPE_ATTR_INT, 0);
  nv.v.i[0] = val;
  add_attr (s, entry, nv);
}

void
add_attr_special_int (Attrstore *s, unsigned int entry, Id name, unsigned int val)
{
  if (val > (TYPE_ATTR_SPECIAL_END - TYPE_ATTR_SPECIAL_START))
    add_attr_int (s, entry, name, val);
  else
    {
      LongNV nv;
      nv.key = add_key (s, name, TYPE_ATTR_SPECIAL_START + val, 0);
      add_attr (s, entry, nv);
    }
}

static void
add_attr_chunk (Attrstore *s, unsigned int entry, Id name, unsigned int ofs, unsigned int len)
{
  LongNV nv;
  nv.key = add_key (s, name, TYPE_ATTR_CHUNK, 0);
  nv.v.i[0] = ofs;
  nv.v.i[1] = len;
  add_attr (s, entry, nv);
}

void
add_attr_blob (Attrstore *s, unsigned int entry, Id name, const void *ptr, unsigned int len)
{
  if (((s->blob_next_free + BLOB_BLOCK) & ~BLOB_BLOCK)
      != ((s->blob_next_free + len + BLOB_BLOCK) & ~BLOB_BLOCK))
    {
      unsigned int blobsz = (s->blob_next_free + len + BLOB_BLOCK) &~BLOB_BLOCK;
      s->blob_store = xrealloc (s->blob_store, blobsz);
    }
  memcpy (s->blob_store + s->blob_next_free, ptr, len);
  add_attr_chunk (s, entry, name, s->blob_next_free, len);
  s->blob_next_free += len;

  unsigned int npages = (s->blob_next_free + BLOB_PAGESIZE - 1) / BLOB_PAGESIZE;
  if (npages != s->num_pages)
    {
      Attrblobpage *p;
      s->pages = xrealloc (s->pages, npages * sizeof (s->pages[0]));
      for (p = s->pages + s->num_pages; s->num_pages < npages;
	   p++, s->num_pages++)
	{
	  p->mapped_at = s->num_pages * BLOB_PAGESIZE;
	  p->file_offset = 0;
	  p->file_size = 0;
	}
    }
}

void
add_attr_string (Attrstore *s, unsigned int entry, Id name, const char *val)
{
  LongNV nv;
  nv.key = add_key (s, name, TYPE_ATTR_STRING, 0);
  nv.v.str = strdup (val);
  add_attr (s, entry, nv);
}

void
add_attr_intlist_int (Attrstore *s, unsigned int entry, Id name, int val)
{
  LongNV *nv = find_attr (s, entry, name);
  if (val == 0)
    return;
  if (nv)
    {
      unsigned len = 0;
      while (nv->v.intlist[len])
        len++;
      nv->v.intlist = realloc (nv->v.intlist, (len + 2) * sizeof (nv->v.intlist[0]));
      nv->v.intlist[len] = val;
      nv->v.intlist[len+1] = 0;
    }
  else
    {
      LongNV mynv;
      mynv.key = add_key (s, name, TYPE_ATTR_INTLIST, 0);
      mynv.v.intlist = malloc (2 * sizeof (mynv.v.intlist[0]));
      mynv.v.intlist[0] = val;
      mynv.v.intlist[1] = 0;
      add_attr (s, entry, mynv);
    }
}

void
add_attr_localids_id (Attrstore *s, unsigned int entry, Id name, LocalId id)
{
  LongNV *nv = find_attr (s, entry, name);
  if (nv)
    {
      unsigned len = 0;
      while (nv->v.localids[len])
        len++;
      nv->v.localids = realloc (nv->v.localids, (len + 2) * sizeof (nv->v.localids[0]));
      nv->v.localids[len] = id;
      nv->v.localids[len+1] = 0;
    }
  else
    {
      LongNV mynv;
      mynv.key = add_key (s, name, TYPE_ATTR_LOCALIDS, 0);
      mynv.v.localids = malloc (2 * sizeof (mynv.v.localids[0]));
      mynv.v.localids[0] = id;
      mynv.v.localids[1] = 0;
      add_attr (s, entry, mynv);
    }
}

void
add_attr_void (Attrstore *s, unsigned int entry, Id name)
{
  LongNV nv;
  nv.key = add_key (s, name, TYPE_VOID, 0);
  add_attr (s, entry, nv);
}

void
merge_attrs (Attrstore *s, unsigned dest, unsigned src)
{
  LongNV *nv;
  ensure_entry (s, dest);
  nv = s->attrs[src];
  if (nv)
    {
      for (; nv->key; nv++)
        if (!find_attr (s, dest, s->keys[nv->key].name))
	  switch (s->keys[nv->key].type)
	    {
	      case TYPE_ATTR_INTLIST:
	        {
		  unsigned len = 0;
		  while (nv->v.intlist[len])
		    add_attr_intlist_int (s, dest, s->keys[nv->key].name, nv->v.intlist[len++]);
		}
		break;
	      case TYPE_ATTR_LOCALIDS:
	        {
		  unsigned len = 0;
		  while (nv->v.localids[len])
		    add_attr_localids_id (s, dest, s->keys[nv->key].name, nv->v.localids[len++]);
		}
		break;
	      case TYPE_ATTR_STRING:
	        add_attr_string (s, dest, s->keys[nv->key].name, nv->v.str);
		break;
	      default:
		add_attr (s, dest, *nv);
		break;
	    }
    }
}

#define pool_debug(a,b,...) fprintf (stderr, __VA_ARGS__)

static Id read_id (FILE *fp, Id max);

/* This routine is used only when attributes are embedded into the
   normal repo SOLV file.  */
void
add_attr_from_file (Attrstore *s, unsigned entry, Id name, int type, Id *idmap, unsigned maxid, FILE *fp)
{
  Pool *pool = s->pool;
  //fprintf (stderr, "%s: attribute in a repo SOLV?\n", id2str (pool, name));
  switch (type)
    {
      case TYPE_VOID:
        add_attr_void (s, entry, name);
	break;
      case TYPE_ATTR_CHUNK:
	{
	  unsigned ofs = read_id (fp, 0);
	  unsigned len = read_id (fp, 0);
	  add_attr_chunk (s, entry, name, ofs, len);
	}
	break;
      case TYPE_ATTR_INT:
	{
	  unsigned i = read_id(fp, 0);
	  add_attr_int (s, entry, name, i);
	}
	break;
      case TYPE_ATTR_STRING:
        {
	  unsigned char localbuf[1024];
	  int c;
	  unsigned char *buf = localbuf;
	  unsigned len = sizeof (localbuf);
	  unsigned ofs = 0;
	  while((c = getc (fp)) != 0)
	    {
	      if (c == EOF)
		{
		  pool_debug (mypool, SAT_FATAL, "unexpected EOF\n");
		  exit (1);
		}
	      /* Plus 1 as we also want to add the 0.  */
	      if (ofs + 1 >= len)
	        {
		  len += 256;
		  if (buf == localbuf)
		    {
		      buf = xmalloc (len);
		      memcpy (buf, localbuf, len - 256);
		    }
		  else
		    buf = xrealloc (buf, len);
		}
	      buf[ofs++] = c;
	    }
	  buf[ofs++] = 0;
	  add_attr_string (s, entry, name, (char*) buf);
	  if (buf != localbuf)
	    xfree (buf);
	}
	break;
      case TYPE_ATTR_INTLIST:
        {
	  unsigned i;
	  while ((i = read_id(fp, 0)) != 0)
	    add_attr_intlist_int (s, entry, name, i);
	}
	break;
      case TYPE_ATTR_LOCALIDS:
        {
	  Id i;
	  /* The read ID will be pool-based.  */
	  while ((i = read_id(fp, maxid)) != 0)
	    {
	      if (idmap)
	        i = idmap[i];
	      add_attr_localids_id (s, entry, name, str2localid (s, id2str (pool, i), 1));
	    }
	}
	break;
      default:
        if (type >= TYPE_ATTR_SPECIAL_START && type <= TYPE_ATTR_SPECIAL_END)
	  {
	    add_attr_special_int (s, entry, name, type - TYPE_ATTR_SPECIAL_START);
	    break;
	  }
	pool_debug(pool, SAT_FATAL, "unknown type %d\n", type);
	exit(0);
    }
}

/* Make sure all pages from PSTART to PEND (inclusive) are loaded,
   and are consecutive.  Return a pointer to the mapping of PSTART.  */
static const void *
load_page_range (Attrstore *s, unsigned int pstart, unsigned int pend)
{
  unsigned char buf[BLOB_PAGESIZE];
  unsigned int i;

  /* Quick check in case all pages are there already and consecutive.  */
  for (i = pstart; i <= pend; i++)
    if (s->pages[i].mapped_at == -1
        || (i > pstart
	    && s->pages[i].mapped_at
	       != s->pages[i-1].mapped_at + BLOB_PAGESIZE))
      break;
  if (i > pend)
    return s->blob_store + s->pages[pstart].mapped_at;

  /* Ensure that we can map the numbers of pages we need at all.  */
  if (pend - pstart + 1 > s->ncanmap)
    {
      unsigned int oldcan = s->ncanmap;
      s->ncanmap = pend - pstart + 1;
      if (s->ncanmap < 4)
        s->ncanmap = 4;
      s->mapped = xrealloc (s->mapped, s->ncanmap * sizeof (s->mapped[0]));
      memset (s->mapped + oldcan, 0, (s->ncanmap - oldcan) * sizeof (s->mapped[0]));
      s->blob_store = xrealloc (s->blob_store, s->ncanmap * BLOB_PAGESIZE);
#ifdef DEBUG_PAGING
      fprintf (stderr, "PAGE: can map %d pages\n", s->ncanmap);
#endif
    }

  /* Now search for "cheap" space in our store.  Space is cheap if it's either
     free (very cheap) or contains pages we search for anyway.  */

  /* Setup cost array.  */
  unsigned int cost[s->ncanmap];
  for (i = 0; i < s->ncanmap; i++)
    {
      unsigned int pnum = s->mapped[i];
      if (pnum == 0)
        cost[i] = 0;
      else
        {
	  pnum--;
	  Attrblobpage *p = s->pages + pnum;
	  assert (p->mapped_at != -1);
	  if (pnum >= pstart && pnum <= pend)
	    cost[i] = 1;
	  else
	    cost[i] = 3;
	}
    }

  /* And search for cheapest space.  */
  unsigned int best_cost = -1;
  unsigned int best = 0;
  unsigned int same_cost = 0;
  for (i = 0; i + pend - pstart < s->ncanmap; i++)
    {
      unsigned int c = cost[i];
      unsigned int j;
      for (j = 0; j < pend - pstart + 1; j++)
        c += cost[i+j];
      if (c < best_cost)
        best_cost = c, best = i;
      else if (c == best_cost)
        same_cost++;
      /* A null cost won't become better.  */
      if (c == 0)
        break;
    }
  /* If all places have the same cost we would thrash on slot 0.  Avoid
     this by doing a round-robin strategy in this case.  */
  if (same_cost == s->ncanmap - pend + pstart - 1)
    best = s->rr_counter++ % (s->ncanmap - pend + pstart);

  /* So we want to map our pages from [best] to [best+pend-pstart].
     Use a very simple strategy, which doesn't make the best use of
     our resources, but works.  Throw away all pages in that range
     (even ours) then copy around ours (in case they were outside the 
     range) or read them in.  */
  for (i = best; i < best + pend - pstart + 1; i++)
    {
      unsigned int pnum = s->mapped[i];
      if (pnum--
          /* If this page is exactly at the right place already,
	     no need to evict it.  */
          && pnum != pstart + i - best)
	{
	  /* Evict this page.  */
#ifdef DEBUG_PAGING
	  fprintf (stderr, "PAGE: evict page %d from %d\n", pnum, i);
#endif
	  cost[i] = 0;
	  s->mapped[i] = 0;
	  s->pages[pnum].mapped_at = -1;
	}
    }

  /* Everything is free now.  Read in the pages we want.  */
  for (i = pstart; i <= pend; i++)
    {
      Attrblobpage *p = s->pages + i;
      unsigned int pnum = i - pstart + best;
      void *dest = s->blob_store + pnum * BLOB_PAGESIZE;
      if (p->mapped_at != -1)
        {
	  if (p->mapped_at != pnum * BLOB_PAGESIZE)
	    {
#ifdef DEBUG_PAGING
	      fprintf (stderr, "PAGECOPY: %d to %d\n", i, pnum);
#endif
	      /* Still mapped somewhere else, so just copy it from there.  */
	      memcpy (dest, s->blob_store + p->mapped_at, BLOB_PAGESIZE);
	      s->mapped[p->mapped_at / BLOB_PAGESIZE] = 0;
	    }
	}
      else
        {
	  unsigned int in_len = p->file_size;
	  unsigned int compressed = in_len & 1;
	  in_len >>= 1;
#ifdef DEBUG_PAGING
	  fprintf (stderr, "PAGEIN: %d to %d", i, pnum);
#endif
	  /* Not mapped, so read in this page.  */
	  if (fseek (s->file, p->file_offset, SEEK_SET) < 0)
	    {
	      perror ("mapping fseek");
	      exit (1);
	    }
	  if (fread (compressed ? buf : dest, in_len, 1, s->file) != 1)
	    {
	      perror ("mapping fread");
	      exit (1);
	    }
	  if (compressed)
	    {
	      unsigned int out_len;
	      out_len = unchecked_decompress_buf (buf, in_len,
						  dest, BLOB_PAGESIZE);
	      if (out_len != BLOB_PAGESIZE
	          && i < s->num_pages - 1)
	        {
	          fprintf (stderr, "can't decompress\n");
	          exit (1);
		}
#ifdef DEBUG_PAGING
	      fprintf (stderr, " (expand %d to %d)", in_len, out_len);
#endif
	    }
#ifdef DEBUG_PAGING
	  fprintf (stderr, "\n");
#endif
	}
      p->mapped_at = pnum * BLOB_PAGESIZE;
      s->mapped[pnum] = i + 1;
    }

  return s->blob_store + best * BLOB_PAGESIZE;
}

const void *
attr_retrieve_blob (Attrstore *s, unsigned int ofs, unsigned int len)
{
  if (s->file)
    {
      /* Paging!  Yeah!  */
      unsigned int pstart = ofs / BLOB_PAGESIZE;
      unsigned int pend = (ofs + len - 1) / BLOB_PAGESIZE;
      const void *m = load_page_range (s, pstart, pend);
      return m + (ofs & (BLOB_PAGESIZE - 1));
    }
  if (!s->blob_store)
    return 0;
  if (ofs >= s->blob_next_free)
    return 0;
  return s->blob_store + ofs;
}

#define FLAT_ATTR_BLOCK 127
#define KEY_BLOCK 127
#define SCHEMA_BLOCK 127

#define add_elem(buf,ofs,val,block) do { \
  if (((ofs) & (block)) == 0) \
    buf = xrealloc (buf, ((ofs) + (block) + 1) * sizeof((buf)[0])); \
  (buf)[(ofs)++] = val; \
} while (0)
#define add_u16(buf,ofs,val,block) do {\
  typedef int __wrong_buf__[(1-sizeof((buf)[0])) * (sizeof((buf)[0])-1)];\
  add_elem(buf,ofs,(val) & 0xFF,block); \
  add_elem(buf,ofs,((val) >> 8) & 0xFF,block); \
} while (0)
#define add_num(buf,ofs,val,block) do {\
  typedef int __wrong_buf__[(1-sizeof((buf)[0])) * (sizeof((buf)[0])-1)];\
  if ((val) >= (1 << 14)) \
    { \
      if ((val) >= (1 << 28)) \
	add_elem (buf,ofs,((val) >> 28) | 128, block); \
      if ((val) >= (1 << 21)) \
	add_elem (buf,ofs,((val) >> 21) | 128, block); \
      add_elem (buf,ofs,((val) >> 14) | 128, block); \
    } \
  if ((val) >= (1 << 7)) \
    add_elem (buf,ofs,((val) >> 7) | 128, block); \
  add_elem (buf,ofs,(val) & 127, block); \
} while (0)

static int
longnv_cmp (const void *pa, const void *pb)
{
  const LongNV *a = (const LongNV *)pa;
  const LongNV *b = (const LongNV *)pb;
  return a->key - b->key;
}

static Id
add_key (Attrstore *s, Id name, unsigned type, unsigned size)
{
  unsigned i;
  for (i = 0; i < s->nkeys; i++)
    if (s->keys[i].name == name && s->keys[i].type == type)
      break;
  if (i < s->nkeys)
    {
      s->keys[i].size += size;
      return i;
    }
  if ((s->nkeys & KEY_BLOCK) == 0)
    s->keys = xrealloc (s->keys, (s->nkeys + KEY_BLOCK + 1) * sizeof (s->keys[0]));
  s->keys[i].name = name;
  s->keys[i].type = type;
  s->keys[i].size = size;
  return s->nkeys++;
}

void
attr_store_pack (Attrstore *s)
{
  unsigned i;
  unsigned int old_mem = 0;
  if (s->packed)
    return;
  s->ent2attr = xcalloc (s->entries, sizeof (s->ent2attr[0]));
  s->flat_attrs = 0;
  s->attr_next_free = 0;
  s->nschemata = 0;
  s->szschemata = 0;
  s->schemata = 0;
  s->schemaofs = 0;

  add_num (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
  add_elem (s->schemata, s->szschemata, 0, SCHEMA_BLOCK);
  add_elem (s->schemaofs, s->nschemata, 0, SCHEMA_BLOCK);

  for (i = 0; i < s->entries; i++)
    {
      unsigned int num_attrs = 0, ofs;
      LongNV *nv = s->attrs[i];
      if (nv)
        while (nv->key)
	  nv++, num_attrs++;
      if (nv)
        old_mem += (num_attrs + 1) * sizeof (LongNV);
      if (!num_attrs)
        continue;
      nv = s->attrs[i];
      qsort (s->attrs[i], num_attrs, sizeof (LongNV), longnv_cmp);
      unsigned int this_schema;
      for (this_schema = 0; this_schema < s->nschemata; this_schema++)
        {
	  for (ofs = 0; ofs < num_attrs; ofs++)
	    {
	      Id key = nv[ofs].key;
	      assert (s->schemaofs[this_schema] + ofs < s->szschemata);
	      if (key != s->schemata[s->schemaofs[this_schema]+ofs])
		break;
	    }
	  if (ofs == num_attrs && !s->schemata[s->schemaofs[this_schema]+ofs])
	    break;
	}
      if (this_schema == s->nschemata)
        {
	  /* This schema not found --> insert it.  */
	  add_elem (s->schemaofs, s->nschemata, s->szschemata, SCHEMA_BLOCK);
	  for (ofs = 0; ofs < num_attrs; ofs++)
	    {
	      Id key = nv[ofs].key;
	      add_elem (s->schemata, s->szschemata, key, SCHEMA_BLOCK);
	    }
	  add_elem (s->schemata, s->szschemata, 0, SCHEMA_BLOCK);
	}
      s->ent2attr[i] = s->attr_next_free;
      add_num (s->flat_attrs, s->attr_next_free, this_schema, FLAT_ATTR_BLOCK);
      for (ofs = 0; ofs < num_attrs; ofs++)
	switch (s->keys[nv[ofs].key].type)
	  {
	    case TYPE_VOID:
	      break;
	    case TYPE_ATTR_INT:
	      {
	        unsigned int i = nv[ofs].v.i[0];
		add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
	        break;
	      }
	    case TYPE_ATTR_CHUNK:
	      {
	        unsigned int i = nv[ofs].v.i[0];
		add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
		i = nv[ofs].v.i[1];
		add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
	        break;
	      }
	    case TYPE_ATTR_STRING:
	      {
	        const char *str = nv[ofs].v.str;
		for (; *str; str++)
		  add_elem (s->flat_attrs, s->attr_next_free, *str, FLAT_ATTR_BLOCK);
		add_elem (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
		old_mem += strlen ((const char*)nv[ofs].v.str) + 1;
		xfree ((void*)nv[ofs].v.str);
		break;
	      }
	    case TYPE_ATTR_INTLIST:
	      {
		const int *il = nv[ofs].v.intlist;
		int i;
		for (; (i = *il) != 0; il++, old_mem += 4)
		  add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
		add_num (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
		old_mem+=4;
		xfree (nv[ofs].v.intlist);
	        break;
	      }
	    case TYPE_ATTR_LOCALIDS:
	      {
		const Id *il = nv[ofs].v.localids;
		Id i;
		for (; (i = *il) != 0; il++, old_mem += 4)
		  add_num (s->flat_attrs, s->attr_next_free, i, FLAT_ATTR_BLOCK);
		add_num (s->flat_attrs, s->attr_next_free, 0, FLAT_ATTR_BLOCK);
		old_mem += 4;
		xfree (nv[ofs].v.localids);
	        break;
	      }
	    default:
	      break;
	  }
      xfree (nv);
    }
  old_mem += s->entries * sizeof (s->attrs[0]);
  free (s->attrs);
  s->attrs = 0;

  /* Remove the hashtable too, it will be build on demand in str2localid
     the next time we call it, which should not happen while in packed mode.  */
  old_mem += (s->ss.stringhashmask + 1) * sizeof (s->ss.stringhashtbl[0]);
  free (s->ss.stringhashtbl);
  s->ss.stringhashtbl = 0;
  s->ss.stringhashmask = 0;

  fprintf (stderr, "%d\n", old_mem);
  fprintf (stderr, "%zd\n", s->entries * sizeof(s->ent2attr[0]));
  fprintf (stderr, "%d\n", s->attr_next_free);
  fprintf (stderr, "%zd\n", s->nschemata * sizeof(s->schemaofs[0]));
  fprintf (stderr, "%zd\n", s->szschemata * sizeof(s->schemata[0]));
  fprintf (stderr, "pages %d\n", s->num_pages);
  s->packed = 1;
}

/* Pages in all blob pages, and deactivates paging.  */
static void
pagein_all (Attrstore *s)
{
  /* If we have no backing file everything is there already.  */
  if (!s->file)
    return;
  /*fprintf (stderr, "Aieee!\n");
  exit (1);*/
}

void
attr_store_unpack (Attrstore *s)
{
  unsigned int i;
  if (!s->packed)
    return;

  pagein_all (s);

  /* Make the store writable right away, so we can use our adder functions.  */
  s->packed = 0;
  s->attrs = xcalloc (s->entries, sizeof (s->attrs[0]));

  for (i = 0; i < s->entries; i++)
    {
      attr_iterator ai;
      FOR_ATTRS (s, i, &ai)
        {
	  switch (ai.type)
	    {
	    case TYPE_VOID:
	      add_attr_void (s, i, ai.name);
	      break;
	    case TYPE_ATTR_INT:
	      add_attr_int (s, i, ai.name, ai.as_int); 
	      break;
	    case TYPE_ATTR_CHUNK:
	      add_attr_chunk (s, i, ai.name, ai.as_chunk[0], ai.as_chunk[1]);
	      break;
	    case TYPE_ATTR_STRING:
	      add_attr_string (s, i, ai.name, ai.as_string);
	      break;
	    case TYPE_ATTR_INTLIST:
	      {
		while (1)
		  {
		    int val;
		    get_num (ai.as_numlist, val);
		    if (!val)
		      break;
		    add_attr_intlist_int (s, i, ai.name, val);
		  }
	        break;
	      }
	    case TYPE_ATTR_LOCALIDS:
	      {
		while (1)
		  {
		    Id val;
		    get_num (ai.as_numlist, val);
		    if (!val)
		      break;
		    add_attr_localids_id (s, i, ai.name, val);
		  }
	        break;
	      }
	    default:
	      if (ai.type >= TYPE_ATTR_SPECIAL_START
	          && ai.type <= TYPE_ATTR_SPECIAL_END)
		add_attr_special_int (s, i, ai.name, ai.type - TYPE_ATTR_SPECIAL_START);
	      break;
	    }
	}
    }

  xfree (s->ent2attr);
  s->ent2attr = 0;
  xfree (s->flat_attrs);
  s->flat_attrs = 0;
  s->attr_next_free = 0;
  xfree (s->schemaofs);
  s->schemaofs = 0;
  s->nschemata = 0;
  xfree (s->schemata);
  s->schemata = 0;
  s->szschemata = 0;
}

static void
write_u8(FILE *fp, unsigned int x)
{
  if (putc(x, fp) == EOF)
    {
      perror("write error");
      exit(1);
    }
}

static void
write_u32(FILE *fp, unsigned int x)
{
  if (putc(x >> 24, fp) == EOF ||
      putc(x >> 16, fp) == EOF ||
      putc(x >> 8, fp) == EOF ||
      putc(x, fp) == EOF)
    {
      perror("write error");
      exit(1);
    }
}

static void
write_id(FILE *fp, Id x)
{
  if (x >= (1 << 14))
    {
      if (x >= (1 << 28))
	putc((x >> 28) | 128, fp);
      if (x >= (1 << 21))
	putc((x >> 21) | 128, fp);
      putc((x >> 14) | 128, fp);
    }
  if (x >= (1 << 7))
    putc((x >> 7) | 128, fp);
  if (putc(x & 127, fp) == EOF)
    {
      perror("write error");
      exit(1);
    }
}

static Id *
write_idarray(FILE *fp, Id *ids)
{
  Id id;
  if (!ids)
    return ids;
  if (!*ids)
    {
      write_u8(fp, 0);
      return ids + 1;
    }
  for (;;)
    {
      id = *ids++;
      if (id >= 64)
	id = (id & 63) | ((id & ~63) << 1);
      if (!*ids)
	{
	  write_id(fp, id);
	  return ids + 1;
	}
      write_id(fp, id | 64);
    }
}

static void
write_pages (FILE *fp, Attrstore *s)
{
  unsigned int i;
  unsigned char buf[BLOB_PAGESIZE];

  /* The compressed pages in the file have different sizes, so we need
     to store these sizes somewhere, either in front of all page data,
     interleaved with the page data (in front of each page), or after
     the page data.  At this point we don't yet know the final compressed
     sizes.  These are the pros and cons:
     * in front of all page data
       + when reading back we only have to read this header, and know
         where every page data is placed
       - we have to compress all pages first before starting to write them.
         Our output stream might be unseekable, so we can't simply
	 reserve space for the header, write all pages and then update the
	 header.  This needs memory for all compressed pages at once.
     * interleaved with page data
       + we can compress and write per page, low memory overhead
       - when reading back we have to read at least those numbers,
         thereby either having to read all page data, or at least seek
	 over it.
     * after all page data
       + we can do streamed writing, remembering the sizes per page,
         and emitting the header (which is a footer then) at the end
       - reading back is hardest: before the page data we don't know
         how long it is overall, so we have to put that information
	 also at the end, but it needs a determinate position, so can
	 only be at a known offset from the end.  But that means that
	 we must be able to seek when reading back.  We have this
	 wish anyway in case we want to use on-demand paging then, but
	 it's optional.

     Of all these it seems the best good/bad ratio is with the interleaved
     storage.  No memory overhead at writing and no unreasonable limitations
     for read back.  */
  write_u32 (fp, s->blob_next_free);
  write_u32 (fp, BLOB_PAGESIZE);
  assert (((s->blob_next_free + BLOB_PAGESIZE - 1) / BLOB_PAGESIZE) == s->num_pages);
  for (i = 0; i < s->num_pages; i++)
    {
      unsigned int in_len;
      unsigned int out_len;
      const void *in;
      if (i == s->num_pages - 1)
        in_len = s->blob_next_free & (BLOB_PAGESIZE - 1);
      else
        in_len = BLOB_PAGESIZE;
      if (in_len)
        {
          in = attr_retrieve_blob (s, i * BLOB_PAGESIZE, in_len);
          out_len = compress_buf (in, in_len, buf, in_len - 1);
          if (!out_len)
            {
              memcpy (buf, in, in_len);
              out_len = in_len;
	    }
	}
      else
        out_len = 0;
#ifdef DEBUG_PAGING
      fprintf (stderr, "page %d: %d -> %d\n", i, in_len, out_len);
#endif
      write_u32 (fp, out_len * 2 + (out_len != in_len));
      if (out_len
          && fwrite (buf, out_len, 1, fp) != 1)
        {
	  perror("write error");
	  exit(1);
	}
    }
}

void
write_attr_store (FILE *fp, Attrstore *s)
{
  unsigned i;
  unsigned local_ssize;

  attr_store_pack (s);

  /* Transform our attribute names (pool string IDs) into local IDs.  */
  for (i = 1; i < s->nkeys; i++)
    s->keys[i].name = str2localid (s, id2str (s->pool, s->keys[i].name), 1);

  /* write file header */
  write_u32(fp, 'S' << 24 | 'O' << 16 | 'L' << 8 | 'V');
  write_u32(fp, SOLV_VERSION_2);

  /* write counts */
  write_u32(fp, s->ss.nstrings);      // nstrings
  write_u32(fp, 0);		      // nrels
  write_u32(fp, s->entries);	      // nsolvables
  write_u32(fp, s->nkeys);
  write_u32(fp, s->nschemata);
  write_u32(fp, 0);	/* no info block */
  unsigned solv_flags = 0;
  solv_flags |= SOLV_FLAG_PACKEDSIZES;
  //solv_flags |= SOLV_FLAG_PREFIX_POOL;
  write_u32(fp, solv_flags);

  for (i = 1, local_ssize = 0; i < (unsigned)s->ss.nstrings; i++)
    local_ssize += strlen (localid2str (s, i)) + 1;

  write_u32 (fp, local_ssize);
  for (i = 1; i < (unsigned)s->ss.nstrings; i++)
    {
      const char *str = localid2str (s, i);
      if (fwrite(str, strlen(str) + 1, 1, fp) != 1)
	{
	  perror("write error");
	  exit(1);
	}
    }

  for (i = 1; i < s->nkeys; i++)
    {
      write_id (fp, s->keys[i].name);
      write_id (fp, s->keys[i].type);
      write_id (fp, s->keys[i].size);

      /* Also transform back the names (now local IDs) into pool IDs,
	 so we can use the pool also after writing.  */
      s->keys[i].name = str2id (s->pool, localid2str (s, s->keys[i].name), 0);
    }

  write_id (fp, s->szschemata);
  Id *ids = s->schemata + 0;
  for (i = 0; i < s->nschemata; i++)
    ids = write_idarray (fp, ids);
  assert (ids == s->schemata + s->szschemata);

  /* Convert our offsets into sizes.  */
  unsigned end = s->attr_next_free;
  for (i = s->entries; i > 0;)
    {
      i--;
      if (s->ent2attr[i])
        {
          s->ent2attr[i] = end - s->ent2attr[i];
	  end = end - s->ent2attr[i];
	}
    }
  /* The first zero should not have been consumed, but everything else.  */
  assert (end == 1);
  /* Write the sizes and convert back to offsets.  */
  unsigned start = 1;
  for (i = 0; i < s->entries; i++)
    {
      write_id (fp, s->ent2attr[i]);
      if (s->ent2attr[i])
        s->ent2attr[i] += start, start = s->ent2attr[i];
    }

  if (s->entries
      && fwrite (s->flat_attrs + 1, s->attr_next_free - 1, 1, fp) != 1)
    {
      perror ("write error");
      exit (1);
    }

  write_pages (fp, s);
}

static unsigned int
read_u32(FILE *fp)
{
  int c, i;
  unsigned int x = 0;

  for (i = 0; i < 4; i++)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  fprintf(stderr, "unexpected EOF\n");
	  exit(1);
	}
      x = (x << 8) | c;
    }
  return x;
}

static Id
read_id(FILE *fp, Id max)
{
  unsigned int x = 0;
  int c, i;

  for (i = 0; i < 5; i++)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  fprintf(stderr, "unexpected EOF\n");
	  exit(1);
	}
      if (!(c & 128))
	{
	  x = (x << 7) | c;
	  if (max && x >= max)
	    {
	      fprintf(stderr, "read_id: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  return x;
	}
      x = (x << 7) ^ c ^ 128;
    }
  fprintf(stderr, "read_id: id too long\n");
  exit(1);
}

static Id *
read_idarray(FILE *fp, Id max, Id *map, Id *store, Id *end, int relative)
{
  unsigned int x = 0;
  int c;
  Id old = 0;
  for (;;)
    {
      c = getc(fp);
      if (c == EOF)
	{
	  pool_debug(mypool, SAT_FATAL, "unexpected EOF\n");
	  exit(1);
	}
      if ((c & 128) == 0)
	{
	  x = (x << 6) | (c & 63);
	  if (relative)
	    {
	      if (x == 0 && c == 0x40)
		{
		  /* prereq hack */
		  if (store == end)
		    {
		      pool_debug(mypool, SAT_FATAL, "read_idarray: array overflow\n");
		      exit(1);
		    }
		  *store++ = SOLVABLE_PREREQMARKER;
		  old = 0;
		  x = 0;
		  continue;
		}
	      x = (x - 1) + old;
	      old = x;
	    }
	  if (x >= max)
	    {
	      pool_debug(mypool, SAT_FATAL, "read_idarray: id too large (%u/%u)\n", x, max);
	      exit(1);
	    }
	  if (map)
	    x = map[x];
	  if (store == end)
	    {
	      pool_debug(mypool, SAT_FATAL, "read_idarray: array overflow\n");
	      exit(1);
	    }
	  *store++ = x;
	  if ((c & 64) == 0)
	    {
	      if (x == 0)	/* already have trailing zero? */
		return store;
	      if (store == end)
		{
		  pool_debug(mypool, SAT_FATAL, "read_idarray: array overflow\n");
		  exit(1);
		}
	      *store++ = 0;
	      return store;
	    }
	  x = 0;
	  continue;
	}
      x = (x << 7) ^ c ^ 128;
    }
}

/* Try to either setup on-demand paging (using FP as backing
   file), or in case that doesn't work (FP not seekable) slurps in
   all pages and deactivates paging.  */
void
read_or_setup_pages (FILE *fp, Attrstore *s)
{
  unsigned int blobsz;
  unsigned int pagesz;
  unsigned int npages;
  unsigned int i;
  unsigned int can_seek;
  long cur_file_ofs;
  unsigned char buf[BLOB_PAGESIZE];
  blobsz = read_u32 (fp);
  pagesz = read_u32 (fp);
  if (pagesz != BLOB_PAGESIZE)
    {
      /* We could handle this by slurping in everything.  */
      fprintf (stderr, "non matching page size\n");
      exit (1);
    }
  can_seek = 1;
  if ((cur_file_ofs = ftell (fp)) < 0)
    can_seek = 0;
  clearerr (fp);
  fprintf (stderr, "can %sseek\n", can_seek ? "" : "NOT ");
  npages = (blobsz + BLOB_PAGESIZE - 1) / BLOB_PAGESIZE;

  s->num_pages = npages;
  s->pages = xmalloc (npages * sizeof (s->pages[0]));

  /* If we can't seek on our input we have to slurp in everything.  */
  if (!can_seek)
    {
      s->blob_next_free = blobsz;
      s->blob_store = xrealloc (s->blob_store, (s->blob_next_free + BLOB_BLOCK) &~BLOB_BLOCK);
    }
  for (i = 0; i < npages; i++)
    {
      unsigned int in_len = read_u32 (fp);
      unsigned int compressed = in_len & 1;
      Attrblobpage *p = s->pages + i;
      in_len >>= 1;
#ifdef DEBUG_PAGING
      fprintf (stderr, "page %d: len %d (%scompressed)\n",
      	       i, in_len, compressed ? "" : "not ");
#endif
      if (can_seek)
        {
          cur_file_ofs += 4;
	  p->mapped_at = -1;
	  p->file_offset = cur_file_ofs;
	  p->file_size = in_len * 2 + compressed;
	  if (fseek (fp, in_len, SEEK_CUR) < 0)
	    {
	      perror ("fseek");
	      fprintf (stderr, "can't seek after we thought we can\n");
	      /* We can't fall back to non-seeking behaviour as we already
	         read over some data pages without storing them away.  */
	      exit (1);
	    }
	  cur_file_ofs += in_len;
	}
      else
        {
	  unsigned int out_len;
	  void *dest = s->blob_store + i * BLOB_PAGESIZE;
          p->mapped_at = i * BLOB_PAGESIZE;
	  p->file_offset = 0;
	  p->file_size = 0;
	  /* We can't seek, so suck everything in.  */
	  if (fread (compressed ? buf : dest, in_len, 1, fp) != 1)
	    {
	      perror ("fread");
	      exit (1);
	    }
	  if (compressed)
	    {
	      out_len = unchecked_decompress_buf (buf, in_len,
					          dest, BLOB_PAGESIZE);
	      if (out_len != BLOB_PAGESIZE
	          && i < npages - 1)
	        {
	          fprintf (stderr, "can't decompress\n");
	          exit (1);
	        }
	    }
	}
    }

  if (can_seek)
    {
      /* If we are here we were able to seek to all page
         positions, so activate paging by copying FP into our structure.
	 We dup() the file, so that our callers can fclose() it and we
	 still have it open.  But this means that we share file positions
	 with the input filedesc.  So in case our caller reads it after us,
	 and calls back into us we might change the file position unexpectedly
	 to him.  */
      int fd = dup (fileno (fp));
      if (fd < 0)
        {
	  /* Jeez!  What a bloody system, we can't dup() anymore.  */
	  perror ("dup");
	  exit (1);
	}
      /* XXX we don't close this yet anywhere.  */
      s->file = fdopen (fd, "r");
      if (!s->file)
        {
	  /* My God!  What happened now?  */
	  perror ("fdopen");
	  exit (1);
	}
    }
}

Attrstore *
attr_store_read (FILE *fp, Pool *pool)
{
  unsigned nentries;
  unsigned i;
  unsigned local_ssize;
  unsigned nstrings, nschemata;
  Attrstore *s = new_store (pool);

  if (read_u32(fp) != ('S' << 24 | 'O' << 16 | 'L' << 8 | 'V'))
    {
      pool_debug(pool, SAT_FATAL, "not a SOLV file\n");
      exit(1);
    }
  unsigned solvversion = read_u32(fp);
  switch (solvversion)
    {
      case SOLV_VERSION_2:
        break;
      default:
        pool_debug(pool, SAT_FATAL, "unsupported SOLV version\n");
        exit(1);
    }

  nstrings = read_u32(fp);
  read_u32(fp); //nrels
  nentries = read_u32(fp);
  s->nkeys = read_u32(fp);
  nschemata = read_u32(fp);
  read_u32(fp); //ninfo
  unsigned solvflags = read_u32(fp);
  if (!(solvflags & SOLV_FLAG_PACKEDSIZES))
    {
      pool_debug(pool, SAT_FATAL, "invalid attribute store\n");
      exit (1);
    }

  /* Slightly hacky.  Our local string pool already contains "<NULL>" and
     "".  We write out the "" too, so we have to read over it.  We write it
     out to be compatible with the SOLV file and to not have to introduce
     merging and mapping the string IDs.  */
  local_ssize = read_u32 (fp) - 1;
  char *strsp = (char *)xrealloc(s->ss.stringspace, s->ss.sstrings + local_ssize + 1);
  Offset *str = (Offset *)xrealloc(s->ss.strings, (nstrings) * sizeof(Offset));

  s->ss.stringspace = strsp;
  s->ss.strings = str;
  strsp += s->ss.sstrings;

  unsigned char ignore_char = 1;
  if (fread(&ignore_char, 1, 1, fp) != 1
      || (local_ssize && fread(strsp, local_ssize, 1, fp) != 1)
      || ignore_char != 0)
    {
      perror ("read error while reading strings");
      exit(1);
    }
  strsp[local_ssize] = 0;

  /* Don't build hashtable here, it will be built on demand by str2localid
     should we call that.  */

  strsp = s->ss.stringspace;
  s->ss.nstrings = nstrings;
  for (i = 0; i < nstrings; i++)
    {
      str[i] = strsp - s->ss.stringspace;
      strsp += strlen (strsp) + 1;
    }
  s->ss.sstrings = strsp - s->ss.stringspace;

  s->keys = xrealloc (s->keys, ((s->nkeys + KEY_BLOCK) & ~KEY_BLOCK) * sizeof (s->keys[0]));
  /* s->keys[0] is initialized in new_store.  */
  for (i = 1; i < s->nkeys; i++)
    {
      s->keys[i].name = read_id (fp, nstrings);
      s->keys[i].type = read_id (fp, TYPE_ATTR_TYPE_MAX + 1);
      s->keys[i].size = read_id (fp, 0);

      /* Globalize the attribute names (they are local IDs right now).  */
      s->keys[i].name = str2id (s->pool, localid2str (s, s->keys[i].name), 1);
    }

  s->szschemata = read_id (fp, 0);
  s->nschemata = 0;
  s->schemata = xmalloc (((s->szschemata + SCHEMA_BLOCK) & ~SCHEMA_BLOCK) * sizeof (s->schemata[0]));
  s->schemaofs = 0;
  Id *ids = s->schemata;
  //add_elem (s->schemaofs, s->nschemata, 0, SCHEMA_BLOCK);
  //*ids++ = 0;
  while (ids < s->schemata + s->szschemata)
    {
      add_elem (s->schemaofs, s->nschemata, ids - s->schemata, SCHEMA_BLOCK);
      ids = read_idarray (fp, s->nkeys, 0, ids, s->schemata + s->szschemata, 0);
    }
  assert (ids == s->schemata + s->szschemata);
  assert (nschemata == s->nschemata);

  s->entries = nentries;

  s->ent2attr = xmalloc (s->entries * sizeof (s->ent2attr[0]));
  int start = 1;
  for (i = 0; i < s->entries; i++)
    {
      int d = read_id (fp, 0);
      if (d)
        s->ent2attr[i] = start, start += d;
      else
        s->ent2attr[i] = 0;
    }

  s->attr_next_free = start;
  s->flat_attrs = xmalloc (((s->attr_next_free + FLAT_ATTR_BLOCK) & ~FLAT_ATTR_BLOCK) * sizeof (s->flat_attrs[0]));
  s->flat_attrs[0] = 0;
  if (s->entries && fread (s->flat_attrs + 1, s->attr_next_free - 1, 1, fp) != 1)
    {
      perror ("read error");
      exit (1);
    }

  read_or_setup_pages (fp, s);

  s->packed = 1;

  return s;
}

void
attr_store_search_s (Attrstore *s, const char *pattern, int flags, Id name, cb_attr_search_s cb)
{
  unsigned int i;
  attr_iterator ai;
  regex_t regex;
  /* If we search for a glob, but we don't have a wildcard pattern, make this
     an exact string search.  */
  if ((flags & 7) == SEARCH_GLOB
      && !strpbrk (pattern, "?*["))
    flags = SEARCH_STRING | (flags & ~7);
  if ((flags & 7) == SEARCH_REGEX)
    {
      /* We feed multiple lines eventually (e.g. authors or descriptions),
         so set REG_NEWLINE.  */
      if (regcomp (&regex, pattern,
      		   REG_EXTENDED | REG_NOSUB | REG_NEWLINE
		   | ((flags & SEARCH_NOCASE) ? REG_ICASE : 0)) != 0)
	return;
    }
  for (i = 0; i < s->entries; i++)
    FOR_ATTRS (s, i, &ai)
      {
        const char *str;
        if (name && name != ai.name)
	  continue;
	str = 0;
	switch (ai.type)
	  {
	  case TYPE_ATTR_INT:
	  case TYPE_ATTR_INTLIST:
	    continue;
	  case TYPE_ATTR_CHUNK:
	    if (!(flags & SEARCH_BLOBS))
	      continue;
	    str = attr_retrieve_blob (s, ai.as_chunk[0], ai.as_chunk[1]);
	    break;
	  case TYPE_ATTR_STRING:
	    str = ai.as_string;
	    break;
	  case TYPE_ATTR_LOCALIDS:
	    {
	      Id val;
	      get_num (ai.as_numlist, val);
	      if (val)
		str = localid2str (s, val);
	      break;
	    }
	  default:
	    break;
	  }
	while (str)
	  {
	    unsigned int match = 0;
	    switch (flags & 7)
	    {
	      case SEARCH_SUBSTRING:
	        if (flags & SEARCH_NOCASE)
		  match = !! strcasestr (str, pattern);
		else
		  match = !! strstr (str, pattern);
		break;
	      case SEARCH_STRING:
	        if (flags & SEARCH_NOCASE)
		  match = ! strcasecmp (str, pattern);
		else
		  match = ! strcmp (str, pattern);
	        break;
	      case SEARCH_GLOB:
	        match = ! fnmatch (pattern, str,
				   (flags & SEARCH_NOCASE) ? FNM_CASEFOLD : 0);
		break;
	      case SEARCH_REGEX:
	        match = ! regexec (&regex, str, 0, NULL, 0);
	        break;
	      default:
	        break;
	    }
	    if (match)
	      cb (s, i, ai.name, str);
	    if (ai.type != TYPE_ATTR_LOCALIDS)
	      break;
	    Id val;
	    get_num (ai.as_numlist, val);
	    if (!val)
	      break;
	    str = localid2str (s, val);
	  }
      }
  if ((flags & 7) == SEARCH_REGEX)
    regfree (&regex);
}

#ifdef MAIN
int
main (void)
{
  Pool *pool = pool_create ();
  Attrstore *s = new_store (pool);
  unsigned int id1 = new_entry (s);
  unsigned int id2 = new_entry (s);
  unsigned int id3 = new_entry (s);
  unsigned int id4 = new_entry (s);
  add_attr_int (s, id1, str2id (s, "name1", 1), 42);
  add_attr_chunk (s, id1, str2id (s->pool, "name2", 1), 9876, 1024);
  add_attr_string (s, id1, str2id (s->pool, "name3", 1), "hallo");
  add_attr_int (s, id1, str2id (s->pool, "name1", 1), 43);
  add_attr_intlist_int (s, id1, str2id (s->pool, "intlist1", 1), 3);
  add_attr_intlist_int (s, id1, str2id (s->pool, "intlist1", 1), 14);
  add_attr_intlist_int (s, id1, str2id (s->pool, "intlist1", 1), 1);
  add_attr_intlist_int (s, id1, str2id (s->pool, "intlist1", 1), 59);
  add_attr_localids_id (s, id1, str2id (s->pool, "l_ids1", 1), str2localid (s, "one", 1));
  add_attr_localids_id (s, id1, str2id (s->pool, "l_ids1", 1), str2localid (s, "two", 1));
  add_attr_localids_id (s, id1, str2id (s->pool, "l_ids1", 1), str2localid (s, "three", 1));
  add_attr_localids_id (s, id1, str2id (s->pool, "l_ids2", 1), str2localid (s, "three", 1));
  add_attr_localids_id (s, id1, str2id (s->pool, "l_ids2", 1), str2localid (s, "two", 1));
  add_attr_localids_id (s, id1, str2id (s->pool, "l_ids2", 1), str2localid (s, "one", 1));
  write_attr_store (stdout, s);
  return 0;
}
#endif
