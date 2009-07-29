/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repodata.h
 * 
 */

#ifndef SATSOLVER_REPODATA_H
#define SATSOLVER_REPODATA_H

#include <stdio.h> 

#include "pooltypes.h"
#include "pool.h"
#include "dirpool.h"
#include "repopage.h"

#define SIZEOF_MD5	16
#define SIZEOF_SHA1	20
#define SIZEOF_SHA256	32

struct _Repo;
struct _KeyValue;

typedef struct _Repokey {
  Id name;
  Id type;               /* REPOKEY_TYPE_xxx */
  unsigned int size;
  unsigned int storage; /* KEY_STORAGE_xxx */
} Repokey;

#define KEY_STORAGE_DROPPED             0
#define KEY_STORAGE_SOLVABLE            1
#define KEY_STORAGE_INCORE              2
#define KEY_STORAGE_VERTICAL_OFFSET     3


typedef struct _Repodata {
  struct _Repo *repo;		/* back pointer to repo */

#define REPODATA_AVAILABLE	0
#define REPODATA_STUB		1
#define REPODATA_ERROR		2
#define REPODATA_STORE		3
#define REPODATA_LOADING	4

  int state;			/* available, stub or error */

  void (*loadcallback)(struct _Repodata *);

  int start;			/* start of solvables this repodata is valid for */
  int end;			/* last solvable + 1 of this repodata */

  FILE *fp;			/* file pointer of solv file */
  int error;			/* corrupt solv file */

  Repokey *keys;		/* keys, first entry is always zero */
  unsigned int nkeys;		/* length of keys array */
  unsigned char keybits[32];	/* keyname hash */

  Id *schemata;			/* schema -> offset into schemadata */
  unsigned int nschemata;	/* number of schemata */
  Id *schemadata;		/* schema storage */
  unsigned int schemadatalen;   /* schema storage size */
  Id *schematahash;		/* unification helper */

  Stringpool spool;		/* local string pool */
  int localpool;		/* is local string pool used */

  Dirpool dirpool;		/* local dir pool */

  unsigned char *incoredata;	/* in-core data */
  unsigned int incoredatalen;	/* in-core data used */
  unsigned int incoredatafree;	/* free data len */

  Id mainschema;		/* SOLVID_META schema */
  Id *mainschemaoffsets;	/* SOLVID_META offsets into incoredata */

  Id *incoreoffset;		/* offset for all entries */

  Id *verticaloffset;		/* offset for all verticals, nkeys elements */
  Id lastverticaloffset;	/* end of verticals */

  Repopagestore store;		/* our page store */

  unsigned char *vincore;	/* internal vertical data */
  unsigned int vincorelen;	/* data size */

  Id **attrs;			/* un-internalized attributes */
  Id **xattrs;			/* anonymous handles */
  int nxattrs;			/* number of handles */

  unsigned char *attrdata;	/* their string data space */
  unsigned int attrdatalen;	/* its len */
  Id *attriddata;		/* their id space */
  unsigned int attriddatalen;	/* its len */

  /* array cache to speed up repodata_add functions*/
  Id lasthandle;
  Id lastkey;
  Id lastdatalen;

} Repodata;

#define SOLVID_META		-1
#define SOLVID_POS		-2
#define SOLVID_SUBSCHEMA	-3		/* internal! */


/*-----
 * management functions
 */
void repodata_initdata(Repodata *data, struct _Repo *repo, int localpool);
void repodata_freedata(Repodata *data);

Repodata *repodata_create(struct _Repo *repo, int localpool);
void repodata_free(Repodata *data);


/*
 * key management functions
 */
Id repodata_key2id(Repodata *data, Repokey *key, int create);
static inline Repokey *
repodata_id2key(Repodata *data, Id keyid)
{
  return data->keys + keyid;
}

/*
 * schema management functions
 */
Id repodata_schema2id(Repodata *data, Id *schema, int create);
static inline Id *
repodata_id2schema(Repodata *data, Id schemaid)
{
  return data->schemadata + data->schemata[schemaid];
}

/*
 * data search and access
 */

/* check if there is a chance that the repodata contains data for
 * the specified keyname */
static inline int
repodata_precheck_keyname(Repodata *data, Id keyname)
{
  unsigned char x = data->keybits[(keyname >> 3) & (sizeof(data->keybits) - 1)];
  return x && (x & (1 << (keyname & 7))) ? 1 : 0;
}

/* search key <keyname> (all keys, if keyname == 0) for Id <solvid>
 * Call <callback> for each match */
void repodata_search(Repodata *data, Id solvid, Id keyname, int flags, int (*callback)(void *cbdata, Solvable *s, Repodata *data, Repokey *key, struct _KeyValue *kv), void *cbdata);

/* Make sure the found KeyValue has the "str" field set. Return false
 * if not possible */
int repodata_stringify(Pool *pool, Repodata *data, Repokey *key, struct _KeyValue *kv, int flags);

int repodata_filelistfilter_matches(Repodata *data, const char *str);


/* lookup functions */
Id repodata_lookup_id(Repodata *data, Id solvid, Id keyname);
const char *repodata_lookup_str(Repodata *data, Id solvid, Id keyname);
int repodata_lookup_num(Repodata *data, Id solvid, Id keyname, unsigned int *value);
int repodata_lookup_void(Repodata *data, Id solvid, Id keyname);
const unsigned char *repodata_lookup_bin_checksum(Repodata *data, Id solvid, Id keyname, Id *typep);
int repodata_lookup_idarray(Repodata *data, Id solvid, Id keyname, Queue *q);


/*-----
 * data assignment functions
 */

/*
 * extend the data so that it contains the specified solvables
 * (no longer needed, as the repodata_set functions autoextend)
 */
void repodata_extend(Repodata *data, Id p);
void repodata_extend_block(Repodata *data, Id p, int num);
void repodata_shrink(Repodata *data, int end);

/* internalize freshly set data, so that it is found by the search
 * functions and written out */
void repodata_internalize(Repodata *data);

/* create an anonymous handle. useful for substructures like
 * fixarray/flexarray  */
Id repodata_new_handle(Repodata *data);

/* basic types: void, num, string, Id */
void repodata_set_void(Repodata *data, Id solvid, Id keyname);
void repodata_set_num(Repodata *data, Id solvid, Id keyname, unsigned int num);
void repodata_set_id(Repodata *data, Id solvid, Id keyname, Id id);
void repodata_set_str(Repodata *data, Id solvid, Id keyname, const char *str);
/* create id from string, then set_id */
void repodata_set_poolstr(Repodata *data, Id solvid, Id keyname, const char *str);

/* set numeric constant */
void repodata_set_constant(Repodata *data, Id solvid, Id keyname, unsigned int constant);

/* set Id constant */
void repodata_set_constantid(Repodata *data, Id solvid, Id keyname, Id id);

/* checksum */
void repodata_set_bin_checksum(Repodata *data, Id solvid, Id keyname, Id type,
			       const unsigned char *buf);
void repodata_set_checksum(Repodata *data, Id solvid, Id keyname, Id type,
			   const char *str);

/* directory (for package file list) */
void repodata_add_dirnumnum(Repodata *data, Id solvid, Id keyname, Id dir, Id num, Id num2);
void repodata_add_dirstr(Repodata *data, Id solvid, Id keyname, Id dir, const char *str);


/* Arrays */
void repodata_add_idarray(Repodata *data, Id solvid, Id keyname, Id id);
void repodata_add_poolstr_array(Repodata *data, Id solvid, Id keyname, const char *str);
void repodata_add_fixarray(Repodata *data, Id solvid, Id keyname, Id ghandle);
void repodata_add_flexarray(Repodata *data, Id solvid, Id keyname, Id ghandle);

/* 
 merge attributes from one solvable to another
 works only if the data is not yet internalized
*/
void repodata_merge_attrs(Repodata *data, Id dest, Id src);
void repodata_merge_some_attrs(Repodata *data, Id dest, Id src, Map *keyidmap, int overwrite);

void repodata_create_stubs(Repodata *data);
void repodata_join(Repodata *data, Id joinkey);

/*
 * load all paged data, used to speed up copying in repo_rpmdb
 */
void repodata_disable_paging(Repodata *data);

/* helper functions */
Id repodata_globalize_id(Repodata *data, Id id, int create);
Id repodata_str2dir(Repodata *data, const char *dir, int create);
const char *repodata_dir2str(Repodata *data, Id did, const char *suf);
const char *repodata_chk2str(Repodata *data, Id type, const unsigned char *buf);
void repodata_set_location(Repodata *data, Id solvid, int medianr, const char *dir, const char *file);

#endif /* SATSOLVER_REPODATA_H */
