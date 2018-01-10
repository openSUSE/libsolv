/*
 * Copyright (c) 2018, VMware, Inc. All Rights Reserved.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * db_lmdb
 *
 * bridge for db to lmdb calls
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>

#include "db_lmdb.h"
#include "util.h"

int db_env_open(struct _DB_ENV_ *dbenv, const char *dbpath, u_int32_t flags, int naccess)
{
  int err = 0;

  err = mdb_env_open(dbenv->env, dbpath, 0, naccess);
  bail_on_error(err);

error:
  return err;
}

int db_env_close(struct _DB_ENV_ *dbenv, u_int32_t flags)
{
  int err = 0;

  if(dbenv && dbenv->env)
    mdb_env_close(dbenv->env);

  return err;
}

int db_env_get_open_flags(struct _DB_ENV_ *dbenv, u_int32_t *flags)
{
  *flags = 0;
  return 0;
}

//TODO: vsnprintf
void db_env_errx(struct _DB_ENV_ *dbenv, const char *fmt, ...)
{
  fprintf(stderr, "err\n");
}

int db_env_create (DB_ENV **dbenvp, u_int32_t flags)
{
  DB_ENV *dbenv = NULL;
  MDB_env *mdbenv = NULL;
  int err = 0;

  dbenv = solv_malloc(sizeof(DB_ENV));
  if(!dbenv)
    err = ENOMEM;
  bail_on_error(err);

  err = mdb_env_create(&mdbenv);
  bail_on_error(err);

  err = mdb_env_set_maxreaders(mdbenv, 16);
  bail_on_error(err);

  err = mdb_env_set_maxdbs(mdbenv, 32);
  bail_on_error(err);

  err = mdb_env_set_mapsize(mdbenv, 50*1024*1024);
  bail_on_error(err);

  dbenv->env = mdbenv;
  dbenv->open = db_env_open;
  dbenv->close = db_env_close;
  dbenv->get_open_flags = db_env_get_open_flags;

  *dbenvp = dbenv;
cleanup:
  return err;

error:
  if(mdbenv)
    mdb_env_close(mdbenv);
  solv_free(dbenv);
  goto cleanup;
}

//db TODO: handle db close
int db_close(struct _DB_ *db, u_int32_t flags)
{
  return 0;
}

//TODO: handle dbc_close
int dbc_close(struct _DBC_ *dbc)
{
/*
  if(dbc->txn)
  {
    if(dbc->txn->txn)
      mdb_txn_abort(dbc->txn->txn);
    dbc->txn->txn = NULL;
    solv_free(dbc->txn);
  }
  dbc->txn = NULL;
*/
  return 0;
}

int dbc_c_close(struct _DBC_ *dbc)
{
  if(dbc->cursor)
    mdb_cursor_close(dbc->cursor);
  dbc->cursor = NULL;
  return 0;
}

int dbc_c_get(struct _DBC_ *dbc, struct _DBT_ *key, struct _DBT_ *value, u_int32_t flags)
{
  int err = 0;
  u_int32_t flagsin = flags == DB_SET ? MDB_SET : MDB_NEXT;
  MDB_val mkey, mval;

  if(flagsin == MDB_SET)
  {
    mkey.mv_size = key->size;
    mkey.mv_data = key->data;
  }

  err = mdb_cursor_get(dbc->cursor, &mkey, &mval, flagsin);
  bail_on_error(err);

  if(flagsin == MDB_NEXT)
  {
    key->size = mkey.mv_size;
    key->data = mkey.mv_data;
  }
  value->size = mval.mv_size;
  value->data = mval.mv_data;

cleanup:
  return err;
error:
  goto cleanup;
}

int db_cursor(struct _DB_ *db, struct _DB_TXN_ *txn,
              struct _DBC_ **dbcp, u_int32_t flags)
{
  int err = 0;
  DBC *dbc = NULL;

  DB_TXN *txncurrent = txn ? txn : db->txn;

  if(!txncurrent || !txncurrent->txn)
    err = EINVAL;
  bail_on_error(err);

  dbc = solv_malloc(sizeof(DBC));
  if(!dbc)
    err = ENOMEM;
  bail_on_error(err);

  err = mdb_cursor_open(txncurrent->txn, db->db, &dbc->cursor);
  bail_on_error(err);

  dbc->close = dbc_close;
  dbc->c_close = dbc_c_close;
  dbc->c_get = dbc_c_get;

  *dbcp = dbc;
cleanup:
  return err;
error:
  solv_free(dbc);
  goto cleanup;
}

int db_get(struct _DB_ *db, struct _DB_TXN_ *txn,
           struct _DBT_ *key, struct _DBT_ *value, u_int32_t flags)
{
  return 0;
}

int db_open(struct _DB_ *db, struct _DB_TXN_ *txn,
            const char *file, const char *database,
            DBTYPE dbtype, u_int32_t flags, int mode)
{
  int err = 0;
  DB_TXN *txnp = NULL;
  MDB_dbi dbi = -1;
  DB_TXN *txncurrent = txn;

  if(!txn)
  {
    u_int32_t txnflags = flags & DB_RDONLY ? MDB_RDONLY : 0;
    txnp = solv_malloc(sizeof(DB_TXN));
    if(!txnp)
      err = ENOMEM;
    bail_on_error(err);

    err = mdb_txn_begin(db->env, NULL, txnflags, &txnp->txn);
    bail_on_error(err);

    txncurrent = txnp;
  }

  err = mdb_dbi_open(txncurrent->txn, file, 0, &dbi);
  bail_on_error(err);

  db->txn = txnp;
  db->db = dbi;

cleanup:
  return err;
error:
  solv_free(txnp);
  goto cleanup;
}

//didnt see a way to get byteswapped from lmdb
int db_get_byteswapped(struct _DB_ *db, int *byteswapped)
{
  *byteswapped = 0;
  return 0;
}

int db_create (DB **db, DB_ENV *dbenv, u_int32_t flags)
{
  int err = 0;
  DB *dbp = NULL;

  dbp = solv_malloc(sizeof(DB));
  if(!dbp)
    err = ENOMEM;
  bail_on_error(err);


  dbp->env = dbenv->env;
  dbp->close = db_close;
  dbp->cursor = db_cursor;
  dbp->get = db_get;
  dbp->get_byteswapped = db_get_byteswapped;
  dbp->open = db_open;
  *db = dbp;

cleanup:
  return err;
error:
  solv_free(dbp);
  goto cleanup;
}
