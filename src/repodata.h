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

#define SIZEOF_MD5	16
#define SIZEOF_SHA1	20
#define SIZEOF_SHA256	32

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

#define REPODATA_AVAILABLE	0
#define REPODATA_STUB		1
#define REPODATA_ERROR		2
#define REPODATA_STORE		3
  int state;			/* available, stub or error */

  void (*loadcallback)(struct _Repodata *);
  char *location;		/* E.g. filename or the like */
  char *checksum;		/* Checksum of the file */
  unsigned nchecksum;		/* Length of the checksum */
  unsigned checksumtype;	/* Type of checksum */

  int start;			/* start of solvables this repodata is valid for */
  int end;			/* last solvable + 1 of this repodata */
  int extrastart;
  int nextra;

  FILE *fp;			/* file pointer of solv file */
  int error;			/* corrupt solv file */


  struct _Repokey *keys;	/* keys, first entry is always zero */
  unsigned int nkeys;		/* length of keys array */

  Id *schemata;			/* schema -> offset into schemadata */
  unsigned int nschemata;	/* number of schemata */

  Id *schemadata;		/* schema storage */
  unsigned int schemadatalen;   /* schema storage size */

  Stringpool spool;		/* local string pool */
  int localpool;		/* is local string pool used */

  Dirpool dirpool;		/* local dir pool */

  unsigned char *incoredata;	/* in-core data (flat_attrs) */
  unsigned int incoredatalen;	/* data len (attr_next_free) */
  unsigned int incoredatafree;	/* free data len */

  Id *incoreoffset;		/* offset for all entries (ent2attr) */
  Id *extraoffset;		/* offset for all extra entries */

  Id *verticaloffset;		/* offset for all verticals, nkeys elements */
  Id lastverticaloffset;	/* end of verticals */

  int pagefd;			/* file descriptor of page file */
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

  Id *attrs;			/* un-internalized attributes */
  Id *extraattrs;		/* Same, but for extra objects.  */
  unsigned char *attrdata;	/* their string data space */
  unsigned int attrdatalen;
  Id *attriddata;		/* their id space */
  unsigned int attriddatalen;
  Id **structs;			/* key-value lists */
  unsigned int nstructs;

  /* array cache */
  Id lasthandle;
  Id lastkey;
  Id lastdatalen;

  Id *addedfileprovides;
} Repodata;


/*-----
 * management functions
 */
void repodata_init(Repodata *data, struct _Repo *repo, int localpool);
void repodata_extend(Repodata *data, Id p);
void repodata_extend_extra(Repodata *data, int nextra);
void repodata_extend_block(Repodata *data, Id p, int num);
void repodata_free(Repodata *data);

/* internalize repodata into .solv, required before writing out a .solv file */
void repodata_internalize(Repodata *data);


/*----
 * access functions
 */

/* Search key <keyname> (all keys, if keyname == 0) for Id <entry>
 * <entry> is _relative_ Id for <data>
 * Call <callback> for each match
 */
void repodata_search(Repodata *data, Id entry, Id keyname, int (*callback)(void *cbdata, Solvable *s, Repodata *data, struct _Repokey *key, struct _KeyValue *kv), void *cbdata);

/* lookup functions */
Id repodata_lookup_id(Repodata *data, Id entry, Id keyid);
const char *repodata_lookup_str(Repodata *data, Id entry, Id keyid);
int repodata_lookup_num(Repodata *data, Id entry, Id keyid, unsigned int *value);
int repodata_lookup_void(Repodata *data, Id entry, Id keyid);
const unsigned char *repodata_lookup_bin_checksum(Repodata *data, Id entry, Id keyid, Id *typep);


/*-----
 * data assignment functions
 */

/* Returns a handle for the attributes of ENTRY.  ENTRY >= 0
   corresponds to data associated with a solvable, ENTRY < 0 is
   extra data.  The returned handle is used in the various repodata_set_*
   functions to add attributes to it.  */
Id repodata_get_handle(Repodata *data, Id entry);

/* basic types: void, num, string, Id */

void repodata_set_void(Repodata *data, Id handle, Id keyname);
void repodata_set_num(Repodata *data, Id handle, Id keyname, unsigned int num);
void repodata_set_str(Repodata *data, Id handle, Id keyname, const char *str);
void repodata_set_id(Repodata *data, Id handle, Id keyname, Id id);

/*  */

void repodata_set_poolstr(Repodata *data, Id handle, Id keyname, const char *str);

/* set numeric constant */
void repodata_set_constant(Repodata *data, Id handle, Id keyname, unsigned int constant);

/* set Id constant */
void repodata_set_constantid(Repodata *data, Id handle, Id keyname, Id id);

/* checksum */
void repodata_set_bin_checksum(Repodata *data, Id handle, Id keyname, Id type,
			       const unsigned char *buf);
void repodata_set_checksum(Repodata *data, Id handle, Id keyname, Id type,
			   const char *str);

/* directory (for package file list) */
void repodata_add_dirnumnum(Repodata *data, Id handle, Id keyname, Id dir, Id num, Id num2);
void repodata_add_dirstr(Repodata *data, Id handle, Id keyname, Id dir, const char *str);


/* Arrays */
void repodata_add_idarray(Repodata *data, Id handle, Id keyname, Id id);
void repodata_add_poolstr_array(Repodata *data, Id handle, Id keyname,
				const char *str);
/* Creates a new substructure.  Returns a handle for it (usable with the
   other repodata_{set,add}_* functions.  */
Id repodata_create_struct(Repodata *data, Id handle, Id keyname);

/*-----
 * data management
 */

/* merge attributes */
void repodata_merge_attrs (Repodata *data, Id dest, Id src);

/* */
void repodata_disable_paging(Repodata *data);

/* helper functions */
Id repodata_globalize_id(Repodata *data, Id id);
Id repodata_str2dir(Repodata *data, const char *dir, int create);
const char *repodata_dir2str(Repodata *data, Id did, const char *suf);
const char *repodata_chk2str(Repodata *data, Id type, const unsigned char *buf);

/* internal */
unsigned int repodata_compress_page(unsigned char *, unsigned int, unsigned char *, unsigned int);
void repodata_read_or_setup_pages(Repodata *data, unsigned int pagesz, unsigned int blobsz);

#endif /* SATSOLVER_REPODATA_H */
