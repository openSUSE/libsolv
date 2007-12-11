/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef ATTR_STORE_H
#define ATTR_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#include "poolid.h"

struct _Pool;
struct _Attrstore;
typedef struct _Attrstore Attrstore;
typedef Id LocalId;

Attrstore * new_store (struct _Pool *pool);
unsigned int new_entry (Attrstore *s);
Attrstore * attr_store_read (FILE *fp, struct _Pool *pool);
void ensure_entry (Attrstore *s, unsigned int entry);
void write_attr_store (FILE *fp, Attrstore *s);
void attr_store_pack (Attrstore *s);
void attr_store_unpack (Attrstore *s);

LocalId str2localid (Attrstore *s, const char *str, int create);
const char * localid2str(Attrstore *s, LocalId id);

void add_attr_int (Attrstore *s, unsigned int entry, Id name, unsigned int val);
void add_attr_blob (Attrstore *s, unsigned int entry, Id name, const void *ptr, unsigned int len);
void add_attr_string (Attrstore *s, unsigned int entry, Id name, const char *val);
void add_attr_intlist_int (Attrstore *s, unsigned int entry, Id name, int val);
void add_attr_localids_id (Attrstore *s, unsigned int entry, Id name, LocalId id);
void add_attr_void (Attrstore *s, unsigned int entry, Id name);

const void * attr_retrieve_blob (Attrstore *s, unsigned int ofs, unsigned int len);

#define SEARCH_SUBSTRING 1
#define SEARCH_STRING 2
#define SEARCH_GLOB 3
#define SEARCH_REGEX 4
#define SEARCH_NOCASE 8
#define SEARCH_BLOBS 16
#define SEARCH_IDS 32
typedef int (*cb_attr_search_s) (Attrstore *s, unsigned entry, Id name, const char *str);
void attr_store_search_s (Attrstore *s, const char *pattern, int flags, Id name, cb_attr_search_s cb);
#ifdef __cplusplus
}
#endif

#endif
