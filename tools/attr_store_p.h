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

  unsigned int packed:1;
};
