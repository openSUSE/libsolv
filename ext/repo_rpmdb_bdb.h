/*
 * Copyright (c) 2018 SUSE Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_rpmdb_bdb.h
 *
 * Use BerkeleyDB to access the rpm database
 *
 */


#if !defined(DB_CREATE) && !defined(ENABLE_RPMDB_LIBRPM)
# if defined(SUSE) || defined(HAVE_RPM_DB_H)
#  include <rpm/db.h>
# else
#  include <db.h>
# endif
#endif

#ifdef RPM5
# include <rpm/rpmversion.h>
# if RPMLIB_VERSION <  RPMLIB_VERSION_ENCODE(5,3,_,0,0,_)
#  define RPM_INDEX_SIZE 8	/* rpmdbid + array index */
# else
#  define RPM_INDEX_SIZE 4	/* just the rpmdbid */
#  define RPM5_BIG_ENDIAN_ID
#endif
#else
# define RPM_INDEX_SIZE 8	/* rpmdbid + array index */
#endif


/******************************************************************/
/*  Rpm Database stuff
 */

struct rpmdbstate {
  Pool *pool;
  char *rootdir;

  RpmHead *rpmhead;	/* header storage space */
  unsigned int rpmheadsize;

  int dbenvopened;	/* database environment opened */
  int pkgdbopened;	/* package database openend */
  const char *dbpath;	/* path to the database */
  int dbpath_allocated;	/* do we need to free the path? */

  DB_ENV *dbenv;	/* database environment */
  DB *db;		/* packages database */
  int byteswapped;	/* endianess of packages database */
  DBC *dbc;		/* iterator over packages database */
};


static inline int
access_rootdir(struct rpmdbstate *state, const char *dir, int mode)
{
  if (state->rootdir)
    {
      char *path = solv_dupjoin(state->rootdir, dir, 0);
      int r = access(path, mode);
      free(path);
      return r;
    }
  return access(dir, mode);
}

static void
detect_dbpath(struct rpmdbstate *state)
{
  state->dbpath = access_rootdir(state, "/var/lib/rpm", W_OK) == -1
                  && access_rootdir(state, "/usr/share/rpm/Packages", R_OK) == 0
                  ? "/usr/share/rpm" : "/var/lib/rpm";
}

static int
stat_database_name(struct rpmdbstate *state, char *dbname, struct stat *statbuf, int seterror)
{
  char *dbpath;
  if (!state->dbpath)
    detect_dbpath(state);
  dbpath = solv_dupjoin(state->rootdir, state->dbpath, dbname);
  if (stat(dbpath, statbuf))
    {
      if (seterror)
        pool_error(state->pool, -1, "%s: %s", dbpath, strerror(errno));
      free(dbpath);
      return -1;
    }
  free(dbpath);
  return 0;
}

static int
stat_database(struct rpmdbstate *state, struct stat *statbuf)
{
  return stat_database_name(state, "/Packages", statbuf, 1);
}


static inline Id
db2rpmdbid(unsigned char *db, int byteswapped)
{
#ifdef RPM5_BIG_ENDIAN_ID
  return db[0] << 24 | db[1] << 16 | db[2] << 8 | db[3];
#else
# if defined(WORDS_BIGENDIAN)
  if (!byteswapped)
# else
  if (byteswapped)
# endif
    return db[0] << 24 | db[1] << 16 | db[2] << 8 | db[3];
  else
    return db[3] << 24 | db[2] << 16 | db[1] << 8 | db[0];
#endif
}

static inline void
rpmdbid2db(unsigned char *db, Id id, int byteswapped)
{
#ifdef RPM5_BIG_ENDIAN_ID
  db[0] = id >> 24, db[1] = id >> 16, db[2] = id >> 8, db[3] = id;
#else
# if defined(WORDS_BIGENDIAN)
  if (!byteswapped)
# else
  if (byteswapped)
# endif
    db[0] = id >> 24, db[1] = id >> 16, db[2] = id >> 8, db[3] = id;
  else
    db[3] = id >> 24, db[2] = id >> 16, db[1] = id >> 8, db[0] = id;
#endif
}

#if defined(FEDORA) || defined(MAGEIA)
static int
serialize_dbenv_ops(struct rpmdbstate *state)
{
  char *lpath;
  mode_t oldmask;
  int fd;
  struct flock fl;

  lpath = solv_dupjoin(state->rootdir, "/var/lib/rpm/.dbenv.lock", 0);
  oldmask = umask(022);
  fd = open(lpath, (O_RDWR|O_CREAT), 0644);
  free(lpath);
  umask(oldmask);
  if (fd < 0)
    return -1;
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  for (;;)
    {
      if (fcntl(fd, F_SETLKW, &fl) != -1)
	return fd;
      if (errno != EINTR)
	break;
    }
  close(fd);
  return -1;
}

#endif

/* should look in /usr/lib/rpm/macros instead, but we want speed... */
static int
opendbenv(struct rpmdbstate *state)
{
  char *dbpath;
  DB_ENV *dbenv = 0;
  int r;

  if (db_env_create(&dbenv, 0))
    return pool_error(state->pool, 0, "db_env_create: %s", strerror(errno));
#if (defined(FEDORA) || defined(MAGEIA)) && (DB_VERSION_MAJOR >= 5 || (DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 5))
  dbenv->set_thread_count(dbenv, 8);
#endif
  state->dbpath = "/var/lib/rpm";
  dbpath = solv_dupjoin(state->rootdir, state->dbpath, 0);
  if (access(dbpath, W_OK) == -1)
    {
      if (access_rootdir(state, "/usr/share/rpm/Packages", R_OK) == 0)
	{
	  state->dbpath = "/usr/share/rpm";
	  free(dbpath);
	  dbpath = solv_dupjoin(state->rootdir, state->dbpath, 0);
	}
      r = dbenv->open(dbenv, dbpath, DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL, 0);
    }
  else
    {
#if defined(FEDORA) || defined(MAGEIA)
      int serialize_fd = serialize_dbenv_ops(state);
      int eflags = DB_CREATE|DB_INIT_CDB|DB_INIT_MPOOL;
      r = dbenv->open(dbenv, dbpath, eflags, 0644);
      /* see rpm commit 2822ccbcdf3e898b960fafb23c4d571e26cef0a4 */
      if (r == DB_VERSION_MISMATCH)
	{
	  eflags |= DB_PRIVATE;
	  dbenv->errx(dbenv, "warning: DB_VERSION_MISMATCH, retrying with DB_PRIVATE");
	  r = dbenv->open(dbenv, dbpath, eflags, 0644);
	}
      if (serialize_fd >= 0)
	close(serialize_fd);
#else
      r = dbenv->open(dbenv, dbpath, DB_CREATE|DB_PRIVATE|DB_INIT_MPOOL, 0);
#endif
    }
  if (r)
    {
      pool_error(state->pool, 0, "dbenv->open: %s", strerror(errno));
      free(dbpath);
      dbenv->close(dbenv, 0);
      return 0;
    }
  free(dbpath);
  state->dbenv = dbenv;
  state->dbenvopened = 1;
  return 1;
}

static void
closedbenv(struct rpmdbstate *state)
{
#if defined(FEDORA) || defined(MAGEIA)
  uint32_t eflags = 0;
#endif

  if (state->db)
    {
      state->db->close(state->db, 0);
      state->db = 0;
    }
  state->pkgdbopened = 0;
  if (!state->dbenv)
    return;
#if defined(FEDORA) || defined(MAGEIA)
  (void)state->dbenv->get_open_flags(state->dbenv, &eflags);
  if (!(eflags & DB_PRIVATE))
    {
      int serialize_fd = serialize_dbenv_ops(state);
      state->dbenv->close(state->dbenv, 0);
      if (serialize_fd >= 0)
	close(serialize_fd);
    }
  else
    state->dbenv->close(state->dbenv, 0);
#else
  state->dbenv->close(state->dbenv, 0);
#endif
  state->dbenv = 0;
  state->dbenvopened = 0;
}

static int
openpkgdb(struct rpmdbstate *state)
{
  if (state->pkgdbopened)
    return state->pkgdbopened > 0 ? 1 : 0;
  state->pkgdbopened = -1;
  if (state->dbenvopened != 1 && !opendbenv(state))
    return 0;
  if (db_create(&state->db, state->dbenv, 0))
    {
      pool_error(state->pool, 0, "db_create: %s", strerror(errno));
      state->db = 0;
      closedbenv(state);
      return 0;
    }
  if (state->db->open(state->db, 0, "Packages", 0, DB_UNKNOWN, DB_RDONLY, 0664))
    {
      pool_error(state->pool, 0, "db->open Packages: %s", strerror(errno));
      state->db->close(state->db, 0);
      state->db = 0;
      closedbenv(state);
      return 0;
    }
  if (state->db->get_byteswapped(state->db, &state->byteswapped))
    {
      pool_error(state->pool, 0, "db->get_byteswapped: %s", strerror(errno));
      state->db->close(state->db, 0);
      state->db = 0;
      closedbenv(state);
      return 0;
    }
  state->pkgdbopened = 1;
  return 1;
}

/* get the rpmdbids of all installed packages from the Name index database.
 * This is much faster then querying the big Packages database */
static struct rpmdbentry *
getinstalledrpmdbids(struct rpmdbstate *state, const char *index, const char *match, int *nentriesp, char **namedatap, int keep_gpg_pubkey)
{
  DB_ENV *dbenv = 0;
  DB *db = 0;
  DBC *dbc = 0;
  int byteswapped;
  DBT dbkey;
  DBT dbdata;
  unsigned char *dp;
  int dl;
  Id nameoff;

  char *namedata = 0;
  int namedatal = 0;
  struct rpmdbentry *entries = 0;
  int nentries = 0;

  *nentriesp = 0;
  if (namedatap)
    *namedatap = 0;

  if (state->dbenvopened != 1 && !opendbenv(state))
    return 0;
  dbenv = state->dbenv;
  if (db_create(&db, dbenv, 0))
    {
      pool_error(state->pool, 0, "db_create: %s", strerror(errno));
      return 0;
    }
  if (db->open(db, 0, index, 0, DB_UNKNOWN, DB_RDONLY, 0664))
    {
      pool_error(state->pool, 0, "db->open %s: %s", index, strerror(errno));
      db->close(db, 0);
      return 0;
    }
  if (db->get_byteswapped(db, &byteswapped))
    {
      pool_error(state->pool, 0, "db->get_byteswapped: %s", strerror(errno));
      db->close(db, 0);
      return 0;
    }
  if (db->cursor(db, NULL, &dbc, 0))
    {
      pool_error(state->pool, 0, "db->cursor: %s", strerror(errno));
      db->close(db, 0);
      return 0;
    }
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  if (match)
    {
      dbkey.data = (void *)match;
      dbkey.size = strlen(match);
    }
  while (dbc->c_get(dbc, &dbkey, &dbdata, match ? DB_SET : DB_NEXT) == 0)
    {
      if (!match && !keep_gpg_pubkey && dbkey.size == 10 && !memcmp(dbkey.data, "gpg-pubkey", 10))
	continue;
      dl = dbdata.size;
      dp = dbdata.data;
      nameoff = namedatal;
      if (namedatap)
	{
	  namedata = solv_extend(namedata, namedatal, dbkey.size + 1, 1, NAMEDATA_BLOCK);
	  memcpy(namedata + namedatal, dbkey.data, dbkey.size);
	  namedata[namedatal + dbkey.size] = 0;
	  namedatal += dbkey.size + 1;
	}
      while(dl >= RPM_INDEX_SIZE)
	{
	  entries = solv_extend(entries, nentries, 1, sizeof(*entries), ENTRIES_BLOCK);
	  entries[nentries].rpmdbid = db2rpmdbid(dp, byteswapped);
	  entries[nentries].nameoff = nameoff;
	  nentries++;
	  dp += RPM_INDEX_SIZE;
	  dl -= RPM_INDEX_SIZE;
	}
      if (match)
	break;
    }
  dbc->c_close(dbc);
  db->close(db, 0);
  /* make sure that enteries is != 0 if there was no error */
  if (!entries)
    entries = solv_extend(entries, 1, 1, sizeof(*entries), ENTRIES_BLOCK);
  *nentriesp = nentries;
  if (namedatap)
    *namedatap = namedata;
  return entries;
}

static int headfromhdrblob(struct rpmdbstate *state, const unsigned char *data, unsigned int size);

/* retrive header by rpmdbid, returns 0 if not found, -1 on error */
static int
getrpm_dbid(struct rpmdbstate *state, Id dbid)
{
  unsigned char buf[4];
  DBT dbkey;
  DBT dbdata;

  if (dbid <= 0)
    return pool_error(state->pool, -1, "illegal rpmdbid %d", dbid);
  if (state->pkgdbopened != 1 && !openpkgdb(state))
    return -1;
  rpmdbid2db(buf, dbid, state->byteswapped);
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  dbkey.data = buf;
  dbkey.size = 4;
  dbdata.data = 0;
  dbdata.size = 0;
  if (state->db->get(state->db, NULL, &dbkey, &dbdata, 0))
    return 0;
  if (!headfromhdrblob(state, (const unsigned char *)dbdata.data, (unsigned int)dbdata.size))
    return -1;
  return dbid;
}

static int
count_headers(struct rpmdbstate *state)
{
  Pool *pool = state->pool;
  struct stat statbuf;
  DB *db = 0;
  DBC *dbc = 0;
  int count = 0;
  DBT dbkey;
  DBT dbdata;

  if (stat_database_name(state, "/Name", &statbuf, 0))
    return 0;
  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  if (db_create(&db, state->dbenv, 0))
    {
      pool_error(pool, 0, "db_create: %s", strerror(errno));
      return 0;
    }
  if (db->open(db, 0, "Name", 0, DB_UNKNOWN, DB_RDONLY, 0664))
    {
      pool_error(pool, 0, "db->open Name: %s", strerror(errno));
      db->close(db, 0);
      return 0;
    }
  if (db->cursor(db, NULL, &dbc, 0))
    {
      db->close(db, 0);
      pool_error(pool, 0, "db->cursor: %s", strerror(errno));
      return 0;
    }
  while (dbc->c_get(dbc, &dbkey, &dbdata, DB_NEXT) == 0)
    count += dbdata.size / RPM_INDEX_SIZE;
  dbc->c_close(dbc);
  db->close(db, 0);
  return count;
}

static int
pkgdb_cursor_open(struct rpmdbstate *state)
{
  if (state->pkgdbopened != 1 && !openpkgdb(state))
    return -1;
  if (state->db->cursor(state->db, NULL, &state->dbc, 0))
    return pool_error(state->pool, -1, "db->cursor failed");
  return 0;
}

static void
pkgdb_cursor_close(struct rpmdbstate *state)
{
  state->dbc->c_close(state->dbc);
  state->dbc = 0;
}

/* retrive header by berkeleydb cursor, returns 0 on EOF, -1 on error */
static Id
pkgdb_cursor_getrpm(struct rpmdbstate *state)
{
  DBT dbkey;
  DBT dbdata;
  Id dbid;

  memset(&dbkey, 0, sizeof(dbkey));
  memset(&dbdata, 0, sizeof(dbdata));
  while (state->dbc->c_get(state->dbc, &dbkey, &dbdata, DB_NEXT) == 0)
    {
      if (dbkey.size != 4)
	return pool_error(state->pool, -1, "corrupt Packages database (key size)");
      dbid = db2rpmdbid(dbkey.data, state->byteswapped);
      if (!dbid)
	continue;	/* ignore join key */
      if (!headfromhdrblob(state, (const unsigned char *)dbdata.data, (unsigned int)dbdata.size))
	return -1;
      return dbid;
    }
  return 0;	/* no more entries */
}

static int
hash_name_index(struct rpmdbstate *state, Chksum *chk)
{
  char *dbpath;
  int fd, l;
  char buf[4096];

  if (!state->dbpath)
    detect_dbpath(state);
  dbpath = solv_dupjoin(state->rootdir, state->dbpath, "/Name");
  if ((fd = open(dbpath, O_RDONLY)) < 0)
    return -1;
  while ((l = read(fd, buf, sizeof(buf))) > 0)
    solv_chksum_add(chk, buf, l);
  close(fd);
  return 0;
}

