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
  Id key;
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

  struct {
    Id name;
    unsigned type;
    unsigned size;
  } *keys;
  unsigned nkeys;
  Id *schemata;
  unsigned *schemaofs;
  unsigned nschemata, szschemata;

  unsigned int packed:1;
};

void add_attr_from_file (Attrstore *s, unsigned entry, Id name, int type, Id *idmap, unsigned maxid, FILE *fp);
void read_or_setup_pages (FILE *fp, Attrstore *s);

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
  Id *schema;
  unsigned char *attrs_next;
  Id name;
  unsigned type;

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
  unsigned int this_schema;
  get_num (ai->attrs, this_schema);
  ai->schema = s->schemata + s->schemaofs[this_schema];

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
  Id key = *(ai->schema);
  if (!key)
    return 0;
  ai->name = s->keys[key].name;
  ai->type = s->keys[key].type;
  ai->attrs_next = ai->attrs;
  switch (ai->type)
    {
    case TYPE_VOID:
      /* No data.  */
      break;
    case TYPE_ATTR_INT:
      {
	int val;
	get_num (ai->attrs_next, val);
	ai->as_int = val;
	break;
      }
    case TYPE_ATTR_CHUNK:
      {
	unsigned int val1, val2;
	get_num (ai->attrs_next, val1);
	ai->as_chunk[0] = val1;
	get_num (ai->attrs_next, val2);
	ai->as_chunk[1] = val2;
	break;
      }
    case TYPE_ATTR_STRING:
      {
	ai->as_string = (const char *) ai->attrs_next;
	ai->attrs_next += strlen ((const char*)ai->attrs_next) + 1;
	break;
      }
    case TYPE_ATTR_INTLIST:
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
    case TYPE_ATTR_LOCALIDS:
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
      /* ??? Convert TYPE_ATTR_SPECIAL_* to _INT type with the right value? */
      break;
    }
  return 1;
}

#define FOR_ATTRS(s,i,ai) \
  for (ai_init (s, i, ai); ai_step (s, ai); (ai)->schema++,(ai)->attrs = (ai)->attrs_next)

#ifdef __cplusplus
}
#endif

#endif
