/*
 * Copyright (c) 2018, VMware, Inc. All Rights Reserved.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef RPM_LMDB_H
#define RPM_LMDB_H

#include <lmdb.h>

typedef enum
{
  DB_UNKNOWN=5
} DBTYPE;

typedef struct _DB_ENV_
{
  MDB_env *env;
  int  (*get_open_flags) (struct _DB_ENV_ *, u_int32_t *);
  int  (*open) (struct _DB_ENV_ *, const char *, u_int32_t, int);
  int  (*close) (struct _DB_ENV_ *, u_int32_t);
  void (*errx) (struct _DB_ENV_ *, const char *, ...);
}DB_ENV;

typedef struct _DB_TXN_
{
  MDB_txn *txn;
}DB_TXN;

typedef struct _DBT_
{
  void *data;
  u_int32_t size;
}DBT;

typedef struct _DBC_
{
  MDB_cursor *cursor;
  int (*close) (struct _DBC_ *);
  int (*c_close) (struct _DBC_ *);
  int (*c_get) (struct _DBC_ *, struct _DBT_ *, struct _DBT_ *, u_int32_t);
}DBC;

typedef struct _DB_
{
  DB_TXN *txn;
  MDB_dbi db;
  MDB_env *env;
  int  (*close) (struct _DB_ *, u_int32_t);
  int  (*cursor)(struct _DB_ *, struct _DB_TXN_ *, struct _DBC_ **, u_int32_t);
  int  (*get) (struct _DB_ *, struct _DB_TXN_ *, struct _DBT_ *, struct _DBT_ *, u_int32_t);
  int  (*get_byteswapped) (struct _DB_ *, int *);
  int  (*open) (struct _DB_ *,
                  struct _DB_TXN_ *,
                  const char *,
                  const char *,
                  DBTYPE,
                  u_int32_t,
                  int);
}DB;

//begin definitions from db.h
#define	DB_CREATE     0x00000001
#define	DB_INIT_CDB   0x00000080
#define	DB_INIT_MPOOL 0x00000400
#define	DB_PRIVATE    0x00010000
#define	DB_RDONLY     0x00000400

#define	DB_NEXT       16
#define	DB_SET        26

#define DB_VERSION_MISMATCH     (-30969)/* Environment version mismatch. */
//end definitions from db.h

int db_env_create (DB_ENV **, u_int32_t);
int db_create (DB **, DB_ENV *, u_int32_t);

#define bail_on_error(nerror) \
    do {                                                           \
        if (nerror)                                               \
        {                                                          \
            goto error;                                            \
        }                                                          \
    } while(0)
#endif//RPM_LMDB_H
