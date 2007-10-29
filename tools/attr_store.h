#ifdef __cplusplus
extern "C" {
#endif

#include "poolid.h"

struct _Pool;
struct _Attrstore;
typedef struct _Attrstore Attrstore;
typedef unsigned short NameId;
typedef Id LocalId;

Attrstore * new_store (struct _Pool *pool);
unsigned int new_entry (Attrstore *s);
Attrstore * attr_store_read (FILE *fp, struct _Pool *pool);
void ensure_entry (Attrstore *s, unsigned int entry);
void write_attr_store (FILE *fp, Attrstore *s);
void attr_store_pack (Attrstore *s);
void attr_store_unpack (Attrstore *s);

NameId  str2nameid (Attrstore *s, const char *str);
LocalId str2localid (Attrstore *s, const char *str, int create);
const char * localid2str(Attrstore *s, LocalId id);

void add_attr_int (Attrstore *s, unsigned int entry, NameId name, unsigned int val);
void add_attr_chunk (Attrstore *s, unsigned int entry, NameId name, unsigned int ofs, unsigned int len);
void add_attr_string (Attrstore *s, unsigned int entry, NameId name, const char *val);
void add_attr_id (Attrstore *s, unsigned int entry, NameId name, Id val);
void add_attr_intlist_int (Attrstore *s, unsigned int entry, NameId name, int val);
void add_attr_localids_id (Attrstore *s, unsigned int entry, NameId name, LocalId id);

#ifdef __cplusplus
}
#endif
