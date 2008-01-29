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

struct _Repo;
struct _Repokey;
struct _KeyValue;

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

typedef struct _Repodata {
  struct _Repo *repo;		/* back pointer to repo */

  int state;			/* available, stub or error */

  void (*loadcallback)(struct _Repodata *);

  int start;			/* start of solvables this repodata is valid for */
  int end;			/* last solvable + 1 of this repodata */

  FILE *fp;			/* file pointer of solv file */
  int error;			/* corrupt solv file */

  struct _Repokey *keys;	/* keys, first entry is always zero */
  unsigned int nkeys;		/* length of keys array */

  Id *schemata;			/* schema -> offset into schemadata */
  unsigned int nschemata;	/* number of schemata */

  Id *schemadata;		/* schema storage */
  unsigned int schemadatalen;   /* schema storage size */

  unsigned char *entryschemau8;	/* schema for entry */
  Id *entryschema;		/* schema for entry */

  Stringpool spool;		/* local string pool */
  int localpool;		/* is local string pool used */

  Dirpool dirpool;		/* local dir pool */

  unsigned char *incoredata;	/* in-core data (flat_attrs) */
  unsigned int incoredatalen;	/* data len (attr_next_free) */
  unsigned int incoredatafree;	/* free data len */

  Id *incoreoffset;		/* offset for all entries (ent2attr) */

  Id *verticaloffset;		/* offset for all verticals, nkeys elements */
  Id lastverticaloffset;	/* end of verticals */

  unsigned char *blob_store;

  Attrblobpage *pages;
  unsigned int num_pages;

  /* mapped[i] is zero if nothing is mapped at logical page I,
     otherwise it contains the pagenumber plus one (of the mapped page).  */
  unsigned int *mapped;
  unsigned int nmapped, ncanmap;
  unsigned int rr_counter;

  unsigned char *vincore;	
  unsigned int vincorelen;

  Id **attrs;			/* un-internalized attributes */
  unsigned char *attrdata;	/* their string data space */
  unsigned int attrdatalen;
  Id *attriddata;		/* their id space */
  unsigned int attriddatalen;

} Repodata;

#define REPODATA_AVAILABLE	0
#define REPODATA_STUB		1
#define REPODATA_ERROR		2
#define REPODATA_STORE		3

void repodata_search(Repodata *data, Id entry, Id keyname, int (*callback)(void *cbdata, Solvable *s, Repodata *data, struct _Repokey *key, struct _KeyValue *kv), void *cbdata);
const char *repodata_lookup_str(Repodata *data, Id entry, Id keyid);

void repodata_extend(Repodata *data, Id p);

void repodata_set_id(Repodata *data, Id entry, Id keyname, Id id);
void repodata_set_num(Repodata *data, Id entry, Id keyname, Id num);
void repodata_set_poolstr(Repodata *data, Id entry, Id keyname, const char *str);
void repodata_set_constant(Repodata *data, Id entry, Id keyname, Id constant);
void repodata_set_void(Repodata *data, Id entry, Id keyname);
void repodata_set_str(Repodata *data, Id entry, Id keyname, const char *str);
void repodata_add_dirnumnum(Repodata *data, Id entry, Id keyname, Id dir, Id num, Id num2);

void repodata_internalize(Repodata *data);

Id repodata_str2dir(Repodata *data, const char *dir, int create);

unsigned int repodata_compress_page(unsigned char *, unsigned int, unsigned char *, unsigned int);
void repodata_read_or_setup_pages(Repodata *data, unsigned int pagesz, unsigned int blobsz);

#endif /* SATSOLVER_REPODATA_H */
