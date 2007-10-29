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

struct _Attrstore
{
  Pool *pool;
  unsigned int entries;
  LongNV **attrs;
  unsigned int num_nameids;
  Id *nameids;
  char *big_store;

  Offset *strings;
  int nstrings;
  char *stringspace;
  Offset sstrings;
  Hashtable stringhashtbl;
  Hashmask stringhashmask;


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
