/*
 * Copyright (c) 2018, SUSE Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_rpmdb_librpm.h
 *
 * Use librpm to access the rpm database
 *
 */

#include <rpm/rpmts.h>
#include <rpm/rpmmacro.h>

struct rpmdbstate {
  Pool *pool;
  char *rootdir;

  RpmHead *rpmhead;	/* header storage space */
  int rpmheadsize;

  int dbenvopened;	/* database environment opened */
  int pkgdbopened;	/* package database openend */
  int is_ostree;	/* read-only db that lives in /usr/share/rpm */

  rpmts ts;
  rpmdbMatchIterator mi;	/* iterator over packages database */
};

static int
stat_database(struct rpmdbstate *state, struct stat *statbuf)
{
  static const char *dbname[] = {
    "Packages",
    "Packages.db",
    "rpmdb.sqlite",
    "data.mdb",
    "Packages",		/* for error reporting */
    0,
  };
  int i;

  for (i = 0; ; i++)
    {
      char *dbpath = solv_dupjoin(state->rootdir, state->is_ostree ? "/usr/share/rpm/" : "/var/lib/rpm/", dbname[i]);
      if (!stat(dbpath, statbuf))
	{
	  free(dbpath);
	  return 0;
	}
      if (errno != ENOENT || !dbname[i + 1])
	{
	  pool_error(state->pool, -1, "%s: %s", dbpath, strerror(errno));
	  solv_free(dbpath);
	  return -1;
	}
      solv_free(dbpath);
    }
  return 0;
}

static int
opendbenv(struct rpmdbstate *state)
{
  const char *rootdir = state->rootdir;
  rpmts ts;
  char *dbpath;
  dbpath = solv_dupjoin("_dbpath ", rootdir, "/var/lib/rpm");
  if (access(dbpath + 8, W_OK) == -1)
    {
      free(dbpath);
      dbpath = solv_dupjoin(rootdir, "/usr/share/rpm/Packages", 0);
      if (access(dbpath, R_OK) == 0)
	state->is_ostree = 1;
      free(dbpath);
      dbpath = solv_dupjoin("_dbpath ", rootdir, state->is_ostree ? "/usr/share/rpm" : "/var/lib/rpm");
    }
  rpmDefineMacro(NULL, dbpath, 0);
  solv_free(dbpath);
  ts = rpmtsCreate();
  if (!ts)
    {
      pool_error(state->pool, 0, "rpmtsCreate failed");
      delMacro(NULL, "_dbpath");
      return 0;
    }
  if (rpmtsOpenDB(ts, O_RDONLY))
    {
      pool_error(state->pool, 0, "rpmtsOpenDB failed: %s", strerror(errno));
      rpmtsFree(ts);
      delMacro(NULL, "_dbpath");
      return 0;
    }
  delMacro(NULL, "_dbpath");
  rpmtsSetVSFlags(ts, _RPMVSF_NODIGESTS | _RPMVSF_NOSIGNATURES | _RPMVSF_NOHEADER);
  state->ts = ts;
  state->dbenvopened = 1;
  state->pkgdbopened = 1;
  return 1;
}

static void
closedbenv(struct rpmdbstate *state)
{
  if (state->ts)
    rpmtsFree(state->ts);
  state->ts = 0;
  state->pkgdbopened = 0;
  state->dbenvopened = 0;
}

static int
openpkgdb(struct rpmdbstate *state)
{
  /* already done in opendbenv */
  return 1;
}

static void
closepkgdb(struct rpmdbstate *state)
{
}

/* get the rpmdbids of all installed packages from the Name index database.
 * This is much faster then querying the big Packages database */
static struct rpmdbentry *
getinstalledrpmdbids(struct rpmdbstate *state, const char *index, const char *match, int *nentriesp, char **namedatap, int keep_gpg_pubkey)
{
  const void * key;
  size_t keylen, matchl = 0;
  Id nameoff;

  char *namedata = 0;
  int namedatal = 0;
  struct rpmdbentry *entries = 0;
  int nentries = 0;

  rpmdbIndexIterator ii;
  int i;

  *nentriesp = 0;
  if (namedatap)
    *namedatap = 0;

  if (state->dbenvopened != 1 && !opendbenv(state))
    return 0;

  if (match)
    matchl = strlen(match);
  ii = rpmdbIndexIteratorInit(rpmtsGetRdb(state->ts), RPMDBI_NAME);

  while (rpmdbIndexIteratorNext(ii, &key, &keylen) == 0)
    {
      if (match)
	{
	  if (keylen != matchl || memcmp(key, match, keylen) != 0)
	    continue;
	}
      else if (!keep_gpg_pubkey && keylen == 10 && !memcmp(key, "gpg-pubkey", 10))
        continue;
      nameoff = namedatal;
      if (namedatap)
       {
         namedata = solv_extend(namedata, namedatal, keylen + 1, 1, NAMEDATA_BLOCK);
         memcpy(namedata + namedatal, key, keylen);
         namedata[namedatal + keylen] = 0;
         namedatal += keylen + 1;
       }
      for (i = 0; i < rpmdbIndexIteratorNumPkgs(ii); i++)
       {
         entries = solv_extend(entries, nentries, 1, sizeof(*entries), ENTRIES_BLOCK);
         entries[nentries].rpmdbid = rpmdbIndexIteratorPkgOffset(ii, i);
         entries[nentries].nameoff = nameoff;
         nentries++;
       }
    }
  rpmdbIndexIteratorFree(ii);
  /* make sure that enteries is != 0 if there was no error */
  if (!entries)
    entries = solv_extend(entries, 1, 1, sizeof(*entries), ENTRIES_BLOCK);
  *nentriesp = nentries;
  if (namedatap)
    *namedatap = namedata;
  return entries;
}

/* retrive header by rpmdbid, returns 0 if not found, -1 on error */
static int
getrpm_dbid(struct rpmdbstate *state, Id rpmdbid)
{
  Header h;
  rpmdbMatchIterator mi;
  unsigned int offset = rpmdbid;

  if (state->dbenvopened != 1 && !opendbenv(state))
    return -1;
  mi = rpmtsInitIterator(state->ts, RPMDBI_PACKAGES, &offset, sizeof(offset));
  h = rpmdbNextIterator(mi);
  if (!h)
    {
      rpmdbFreeIterator(mi);
      return 0;
    }
  if (!rpm_byrpmh(state, h))
    {
      rpmdbFreeIterator(mi);
      return -1;
    }
  mi = rpmdbFreeIterator(mi);
  return 1;
}

static int
count_headers(struct rpmdbstate *state)
{
  int count;
  rpmdbMatchIterator mi;

  if (state->dbenvopened != 1 && !opendbenv(state))
    return 0;
  mi = rpmtsInitIterator(state->ts, RPMDBI_NAME, NULL, 0);
  count = rpmdbGetIteratorCount(mi);
  rpmdbFreeIterator(mi);
  return count;
}

static int
pkgdb_cursor_open(struct rpmdbstate *state)
{
  state->mi = rpmtsInitIterator(state->ts, RPMDBI_PACKAGES, NULL, 0);
  return 0;
}

static void
pkgdb_cursor_close(struct rpmdbstate *state)
{
  rpmdbFreeIterator(state->mi);
  state->mi = 0;
}

static Id
pkgdb_cursor_getrpm(struct rpmdbstate *state)
{
  Header h;
  while ((h = rpmdbNextIterator(state->mi)))
    {
      Id dbid = rpmdbGetIteratorOffset(state->mi);
      if (!rpm_byrpmh(state, h))
	continue;
      return dbid;
    }
  return 0;
}

