/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef ATTR_STORE_P_H
#define ATTR_STORE_P_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  NameId name;
  unsigned type;
#define ATTR_INT      0
#define ATTR_CHUNK    1
#define ATTR_STRING   2
#define ATTR_ID       3
#define ATTR_INTLIST  4
#define ATTR_LOCALIDS 5
  union {
    unsigned int i[2];
    const char *str;
    int *intlist;
    LocalId *localids;
  } v;
} LongNV;

#define BLOB_PAGEBITS 15
#define BLOB_PAGESIZE (1 << BLOB_PAGEBITS)

typedef struct _Attrblobpage
{
  /* mapped_at == -1  --> not loaded, otherwise offset into
     store->blob_store.  The size of the mapping is BLOB_PAGESIZE
     except for the last page.  */
  unsigned int mapped_at;
  long file_offset;
  /* file_size == 0 means the page is not backed by some file storage.
     Otherwise it is L*2+(compressed ? 1 : 0), with L being the data
     length.  */
  long file_size;
} Attrblobpage;

struct _Attrstore
{
  Pool *pool;
  unsigned int entries;
  LongNV **attrs;
  unsigned int num_nameids;
  Id *nameids;
  char *blob_store;
  unsigned int blob_next_free;
  Attrblobpage *pages;
  unsigned int num_pages;
  FILE *file;
  /* mapped[i] is zero if nothing is mapped at logical page I,
     otherwise it contains the pagenumber plus one (of the mapped page).  */
  unsigned int *mapped;
  unsigned int nmapped, ncanmap;

  Stringpool ss;

  /* A space efficient in memory representation.  It's read-only.  */
  /* flat_attrs[ent2attr[i]] are the attrs for entity i.  */
  unsigned int *ent2attr;
  unsigned char *flat_attrs;
  unsigned int attr_next_free;

  /* flat_abbr[abbr[i]] is the schema for abbreviation i.  */
  unsigned int *abbr;
  unsigned int abbr_next_free;
  unsigned short *flat_abbr;
  unsigned int flat_abbr_next_free;

  unsigned int packed:1;
};

#define get_num(ptr,val) do { \
  typedef int __wrong_buf__[(1-sizeof((ptr)[0])) * (sizeof((ptr)[0])-1)];\
  val = 0; \
  while (1) { \
    unsigned char __c = *((ptr)++); \
    (val) = ((val) << 7) | (__c & 0x7F); \
    if ((__c & 0x80) == 0) \
      break; \
  } \
} while (0)

typedef struct {
  unsigned char *attrs;
  unsigned short *abbrp;
  unsigned char *attrs_next;
  NameId name;
  unsigned short type;

  /* The following fields could be a union, but if we do that GCC
     doesn't scalarize these fields anymore, hence real memory accesses
     would remain, so don't do that.  */
  int as_int;
  Id as_id;
  unsigned int as_chunk[2];
  const char *as_string;
  const unsigned char *as_numlist;
} attr_iterator;

static inline void
ai_init (Attrstore *s, unsigned int i, attr_iterator *ai)
{
  ai->attrs = s->flat_attrs + s->ent2attr[i];
  unsigned int this_abbr;
  get_num (ai->attrs, this_abbr);
  ai->abbrp = s->flat_abbr + s->abbr[this_abbr];

  /* Initialize all fields so we get no uninit warnings later.
     Don't use memset() to initialize this structure, it would make
     GCC not scalarize it.  */
  ai->as_int = 0;
  ai->as_id = 0;
  ai->as_chunk[0] = ai->as_chunk[1] = 0;
  ai->as_string = 0;
  ai->as_numlist = 0;
}

static inline int
ai_step (Attrstore *s, attr_iterator *ai)
{
  unsigned short nt = *(ai->abbrp);
  if (!nt)
    return 0;
  ai->name = nt >> 4;
  ai->type = nt & 0xF;
  ai->attrs_next = ai->attrs;
  switch (ai->type)
    {
    case ATTR_INT:
      {
	int val;
	get_num (ai->attrs_next, val);
	ai->as_int = val;
	break;
      }
    case ATTR_ID:
      {
	Id val;
	get_num (ai->attrs_next, val);
	ai->as_id = val;
	break;
      }
    case ATTR_CHUNK:
      {
	unsigned int val1, val2;
	get_num (ai->attrs_next, val1);
	ai->as_chunk[0] = val1;
	get_num (ai->attrs_next, val2);
	ai->as_chunk[1] = val2;
	break;
      }
    case ATTR_STRING:
      {
	ai->as_string = (const char *) ai->attrs_next;
	ai->attrs_next += strlen ((const char*)ai->attrs_next) + 1;
	break;
      }
    case ATTR_INTLIST:
      {
        ai->as_numlist = ai->attrs_next;
	while (1)
	  {
	    int val;
	    get_num (ai->attrs_next, val);
	    if (!val)
	      break;
	  }
	break;
      }
    case ATTR_LOCALIDS:
      {
        ai->as_numlist = ai->attrs_next;
	while (1)
	  {
	    Id val;
	    get_num (ai->attrs_next, val);
	    if (!val)
	      break;
	  }
	break;
      }
    default:
      break;
    }
  return 1;
}

#define FOR_ATTRS(s,i,ai) \
  for (ai_init (s, i, ai); ai_step (s, ai); (ai)->abbrp++,(ai)->attrs = (ai)->attrs_next)

#ifdef __cplusplus
}
#endif

#endif
