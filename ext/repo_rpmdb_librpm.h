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
  int is_ostree;	/* read-only db that lives in /usr/share/rpm */

  rpmts ts;
  rpmdbMatchIterator mi;	/* iterator over packages database */
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
detect_ostree(struct rpmdbstate *state)
{
  state->is_ostree = access_rootdir(state, "/var/lib/rpm", W_OK) == -1 &&
                     access_rootdir(state, "/usr/share/rpm/Packages", R_OK) == 0 ? 1 : -1;
}

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

#ifdef HAVE_RPMDBFSTAT
  if (state->dbenvopened == 1)
    return rpmdbFStat(rpmtsGetRdb(state->ts), statbuf);
#endif
  if (!state->is_ostree)
    detect_ostree(state);
  for (i = 0; ; i++)
    {
      char *dbpath = solv_dupjoin(state->rootdir, state->is_ostree > 0 ? "/usr/share/rpm/" : "/var/lib/rpm/", dbname[i]);
      if (!stat(dbpath, statbuf))
	{
	  free(dbpath);
	  return 0;
	}
      if (errno != ENOENT || !dbname[i + 1])
	{
	  int saved_errno = errno;
	  pool_error(state->pool, -1, "%s: %s", dbpath, strerror(errno));
	  solv_free(dbpath);
	  errno = saved_errno;
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
  detect_ostree(state);
  dbpath = solv_dupjoin("_dbpath ", rootdir, state->is_ostree > 0 ? "/usr/share/rpm" : "/var/lib/rpm");
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
  return 1;
}

static void
closedbenv(struct rpmdbstate *state)
{
  if (state->ts)
    rpmtsFree(state->ts);
  state->ts = 0;
  state->dbenvopened = 0;
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
      unsigned int i, npkgs;
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
      npkgs = rpmdbIndexIteratorNumPkgs(ii);
      for (i = 0; i < npkgs; i++)
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

#if defined(HAVE_RPMDBNEXTITERATORHEADERBLOB) && !defined(ENABLE_RPMPKG_LIBRPM)
static int
rpm_byrpmhdrblob(struct rpmdbstate *state, const unsigned char *data, unsigned int size)
{
  unsigned int dsize, cnt, len;
  RpmHead *rpmhead;
  if (size < 8)
    return pool_error(state->pool, 0, "corrupt rpm database (size)");
  cnt = getu32(data);
  dsize = getu32(data + 4);
  if (cnt >= MAX_HDR_CNT || dsize >= MAX_HDR_DSIZE)
    return pool_error(state->pool, 0, "corrupt rpm database (cnt/dcnt)");
  if (8 + cnt * 16 + dsize > size)
    return pool_error(state->pool, 0, "corrupt rpm database (data size)");
  len = 16 * cnt + dsize;
  if (len + 1 > state->rpmheadsize)
    {
      state->rpmheadsize = len + 128;
      state->rpmhead = solv_realloc(state->rpmhead, sizeof(*state->rpmhead) + state->rpmheadsize);
    }
  rpmhead = state->rpmhead;
  memcpy(rpmhead->data, data + 8, len);
  rpmhead->data[len] = 0;
  rpmhead->cnt = cnt;
  rpmhead->dcnt = dsize;
  rpmhead->dp = rpmhead->data + cnt * 16;
  return 1;
}
#endif

/* retrive header by rpmdbid, returns 0 if not found, -1 on error */
static int
getrpm_dbid(struct rpmdbstate *state, Id rpmdbid)
{
#if defined(HAVE_RPMDBNEXTITERATORHEADERBLOB) && !defined(ENABLE_RPMPKG_LIBRPM)
  const unsigned char *uh;
  unsigned int uhlen;
#else
  Header h;
#endif
  rpmdbMatchIterator mi;
  unsigned int offset = rpmdbid;

  if (rpmdbid <= 0)
    return pool_error(state->pool, -1, "illegal rpmdbid %d", rpmdbid);
  if (state->dbenvopened != 1 && !opendbenv(state))
    return -1;
  mi = rpmdbInitIterator(rpmtsGetRdb(state->ts), RPMDBI_PACKAGES, &offset, sizeof(offset));
#if defined(HAVE_RPMDBNEXTITERATORHEADERBLOB) && !defined(ENABLE_RPMPKG_LIBRPM)
  uh = rpmdbNextIteratorHeaderBlob(mi, &uhlen);
  if (!uh)
    {
      rpmdbFreeIterator(mi);
      return 0;
    }
  if (!rpm_byrpmhdrblob(state, uh, uhlen))
    {
      rpmdbFreeIterator(mi);
      return -1;
    }
#else
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
#endif
  mi = rpmdbFreeIterator(mi);
  return rpmdbid;
}

static int
count_headers(struct rpmdbstate *state)
{
  int count;
  rpmdbMatchIterator mi;

  if (state->dbenvopened != 1 && !opendbenv(state))
    return 0;
  mi = rpmdbInitIterator(rpmtsGetRdb(state->ts), RPMDBI_NAME, NULL, 0);
  count = rpmdbGetIteratorCount(mi);
  rpmdbFreeIterator(mi);
  return count;
}

static int
pkgdb_cursor_open(struct rpmdbstate *state)
{
  state->mi = rpmdbInitIterator(rpmtsGetRdb(state->ts), RPMDBI_PACKAGES, NULL, 0);
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
#if defined(HAVE_RPMDBNEXTITERATORHEADERBLOB) && !defined(ENABLE_RPMPKG_LIBRPM)
  const unsigned char *uh;
  unsigned int uhlen;
  while ((uh = rpmdbNextIteratorHeaderBlob(state->mi, &uhlen)) != 0)
    {
      Id dbid = rpmdbGetIteratorOffset(state->mi);
      if (!rpm_byrpmhdrblob(state, uh, uhlen))
	continue;
      return dbid;
    }
#else
  Header h;
  while ((h = rpmdbNextIterator(state->mi)))
    {
      Id dbid = rpmdbGetIteratorOffset(state->mi);
      if (!rpm_byrpmh(state, h))
	continue;
      return dbid;
    }
#endif
  return 0;
}

static int
hash_name_index(struct rpmdbstate *state, Chksum *chk)
{
  rpmdbIndexIterator ii;
  const void *key;
  size_t keylen;

  if (state->dbenvopened != 1 && !opendbenv(state))
    return -1;
  ii = rpmdbIndexIteratorInit(rpmtsGetRdb(state->ts), RPMDBI_NAME);
  if (!ii)
    return -1;
  while (rpmdbIndexIteratorNext(ii, &key, &keylen) == 0)
    {
      unsigned int i, npkgs = rpmdbIndexIteratorNumPkgs(ii);
      solv_chksum_add(chk, key, (int)keylen);
      for (i = 0; i < npkgs; i++)
	{
	  unsigned int offset = rpmdbIndexIteratorPkgOffset(ii, i);
	  solv_chksum_add(chk, &offset, sizeof(offset));
	}
    }
  rpmdbIndexIteratorFree(ii);
  return 0;
}

