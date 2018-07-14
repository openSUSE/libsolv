/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "hash.h"
#include "chksum.h"
#include "tools_util.h"
#include "repo_susetags.h"
#ifdef ENABLE_COMPLEX_DEPS
#include "pool_parserpmrichdep.h"
#endif
#include "repodata_diskusage.h"

struct datashare {
  Id name;
  Id evr;
  Id arch;
};

struct parsedata {
  int ret;
  Pool *pool;
  Repo *repo;
  Repodata *data;
  char *kind;
  int flags;
  int last_found_source;
  struct datashare *share_with;
  int nshare;
  Queue diskusageq;
  struct joindata jd;
  char *language;			/* the default language */
  Id langcache[ID_NUM_INTERNAL];	/* cache for the default language */
  int lineno;
  char *filelist;
  int afilelist;			/* allocated */
  int nfilelist;			/* used */
};

static char *flagtab[] = {
  ">",
  "=",
  ">=",
  "<",
  "!=",
  "<="
};


static Id
langtag(struct parsedata *pd, Id tag, const char *language)
{
  if (language && *language)
    return pool_id2langid(pd->repo->pool, tag, language, 1);
  if (!pd->language)
    return tag;
  if (tag >= ID_NUM_INTERNAL)
    return pool_id2langid(pd->repo->pool, tag, pd->language, 1);
  if (!pd->langcache[tag])
    pd->langcache[tag] = pool_id2langid(pd->repo->pool, tag, pd->language, 1);
  return pd->langcache[tag];
}

/*
 * adddep
 * create and add dependency
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, Id marker, char *kind)
{
  int i, flags;
  Id id, evrid;
  char *sp[4];

  if (line[6] == '/')
    {
      /* A file dependency. Do not try to parse it */
      id = pool_str2id(pool, line + 6, 1);
    }
#ifdef ENABLE_COMPLEX_DEPS
  else if (line[6] == '(')
    {
      id = pool_parserpmrichdep(pool, line + 6);
      if (!id)
	{
	  pd->ret = pool_error(pool, -1, "susetags: line %d: bad dependency: '%s'\n", pd->lineno, line);
          return olddeps;
	}
    }
#endif
  else
    {
      i = split(line + 6, sp, 4); /* name, <op>, evr, ? */
      if (i != 1 && i != 3) /* expect either 'name' or 'name' <op> 'evr' */
        {
	  pd->ret = pool_error(pool, -1, "susetags: line %d: bad dependency: '%s'\n", pd->lineno, line);
          return olddeps;
        }
      if (kind)
        id = pool_str2id(pool, join2(&pd->jd, kind, ":", sp[0]), 1);
      else
        id = pool_str2id(pool, sp[0], 1);
      if (i == 3)
        {
          evrid = makeevr(pool, sp[2]);
          for (flags = 0; flags < 6; flags++)
            if (!strcmp(sp[1], flagtab[flags]))
              break;
          if (flags == 6)
            {
	      if (!strcmp(sp[1], "<>"))
		flags = 4;
	      else
		{
		  pd->ret = pool_error(pool, -1, "susetags: line %d: unknown relation: '%s'\n", pd->lineno, sp[1]);
		  return olddeps;
		}
            }
          id = pool_rel2id(pool, id, evrid, flags + 1, 1);
        }
    }
  return repo_addid_dep(pd->repo, olddeps, id, marker);
}


/*
 * add_source
 *
 */

static void
add_source(struct parsedata *pd, char *line, Solvable *s, Id handle)
{
  Pool *pool = s->repo->pool;
  char *sp[5];
  Id name;
  Id arch;
  const char *evr, *sevr;

  if (split(line, sp, 5) != 4)
    {
      pd->ret = pool_error(pool, -1, "susetags: line %d: bad source line '%s'\n", pd->lineno, line);
      return;
    }

  name = pool_str2id(pool, sp[0], 1);
  arch = pool_str2id(pool, sp[3], 1);	/* do this before id2str */
  evr = join2(&pd->jd, sp[1], "-", sp[2]);
  sevr = pool_id2str(pool, s->evr);
  if (sevr)
    {
      /* strip epoch */
      const char *p;
      for (p = sevr; *p >= '0' && *p <= '9'; p++)
	;
      if (p != sevr && *p == ':' && p[1])
	sevr = p;
    }
  if (name == s->name)
    repodata_set_void(pd->data, handle, SOLVABLE_SOURCENAME);
  else
    repodata_set_id(pd->data, handle, SOLVABLE_SOURCENAME, name);
  if (sevr && !strcmp(sevr, evr))
    repodata_set_void(pd->data, handle, SOLVABLE_SOURCEEVR);
  else
    repodata_set_id(pd->data, handle, SOLVABLE_SOURCEEVR, pool_str2id(pool, evr, 1));
  repodata_set_constantid(pd->data, handle, SOLVABLE_SOURCEARCH, arch);
}

static void
set_checksum(struct parsedata *pd, Repodata *data, Id handle, Id keyname, char *line)
{
  char *sp[3];
  Id type;
  if (split(line, sp, 3) != 2)
    {
      pd->ret = pool_error(pd->pool, -1, "susetags: line %d: bad checksum line '%s'\n", pd->lineno, line);
      return;
    }
  type = solv_chksum_str2type(sp[0]);
  if (!type)
    {
      pd->ret = pool_error(pd->pool, -1, "susetags: line %d: unknown checksum type: '%s'\n", pd->lineno, sp[0]);
      return;
    }
  if (strlen(sp[1]) != 2 * solv_chksum_len(type))
    {
      pd->ret = pool_error(pd->pool, -1, "susetags: line %d: bad checksum length for type %s: '%s'\n", pd->lineno, sp[0], sp[1]);
      return;
    }
  repodata_set_checksum(data, handle, keyname, type, sp[1]);
}


/* Unfortunately "a"[0] is no constant expression in the C languages,
   so we need to pass the four characters individually :-/  */
#define CTAG(a,b,c,d) ((unsigned)(((unsigned char)a) << 24) \
 | ((unsigned char)b << 16) \
 | ((unsigned char)c << 8) \
 | ((unsigned char)d))

/*
 * tag_from_string
 *
 */

static inline unsigned int
tag_from_string(char *cs)
{
  unsigned char *s = (unsigned char *)cs;
  return ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]);
}


/*
 * repo_add_susetags
 * Parse susetags file passed in fp, fill solvables into repo
 *
 * susetags is key,value based
 *  for short values
 *    =key: value
 *  is used
 *  for long (multi-line) values,
 *    +key:
 *    value
 *    value
 *    -key:
 *  is used
 *
 * See http://en.opensuse.org/Standards/YaST2_Repository_Metadata
 * and http://en.opensuse.org/Standards/YaST2_Repository_Metadata/packages
 * and http://en.opensuse.org/Standards/YaST2_Repository_Metadata/pattern
 *
 * Assumptions:
 *   All keys have 3 characters and end in ':'
 */

static void
finish_solvable(struct parsedata *pd, Solvable *s, Offset freshens)
{
  Pool *pool = pd->repo->pool;
  Id handle = s - pool->solvables;

  if (pd->nfilelist)
    {
      int l;
      Id did;
      for (l = 0; l < pd->nfilelist; l += strlen(pd->filelist + l) + 1)
        {
	  char *p = strrchr(pd->filelist + l, '/');
	  if (!p)
	    continue;
	  *p++ = 0;
	  did = repodata_str2dir(pd->data, pd->filelist + l, 1);
	  p[-1] = '/';
	  if (!did)
	    did = repodata_str2dir(pd->data, "/", 1);
	  repodata_add_dirstr(pd->data, handle, SOLVABLE_FILELIST, did, p);
	}
      pd->nfilelist = 0;
    }
  /* Add self provide, except for source packages.  This is harmless
     to do twice (in case we see the same package twice).  */
  if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  repo_rewrite_suse_deps(s, freshens);
  if (pd->diskusageq.count)
    repodata_add_diskusage(pd->data, handle, &pd->diskusageq);
}

static Hashtable
joinhash_init(Repo *repo, Hashval *hmp)
{
  Hashval hm = mkmask(repo->nsolvables);
  Hashtable ht = solv_calloc(hm + 1, sizeof(*ht));
  Hashval h, hh;
  Solvable *s;
  int i;

  FOR_REPO_SOLVABLES(repo, i, s)
    {
      hh = HASHCHAIN_START;
      h = s->name & hm;
      while (ht[h])
        h = HASHCHAIN_NEXT(h, hh, hm);
      ht[h] = i;
    }
  *hmp = hm;
  return ht;
}

static Solvable *
joinhash_lookup(Repo *repo, Hashtable ht, Hashval hm, Id name, Id evr, Id arch, Id start)
{
  Hashval h, hh;

  if (!name || !arch || !evr)
    return 0;
  hh = HASHCHAIN_START;
  h = name & hm;
  while (ht[h])
    {
      Solvable *s = repo->pool->solvables + ht[h];
      if (ht[h] >= start && s->name == name && s->evr == evr && s->arch == arch)
	return s;
      h = HASHCHAIN_NEXT(h, hh, hm);
    }
  return 0;
}

static Id
lookup_shared_id(Repodata *data, Id p, Id keyname, Id voidid, int uninternalized)
{
  Id r;
  r = repodata_lookup_type(data, p, keyname);
  if (r)
    {
      if (r == REPOKEY_TYPE_VOID)
	return voidid;
      r = repodata_lookup_id(data, p, keyname);
      if (r)
	return r;
    }
  if (uninternalized)
    return repodata_lookup_id_uninternalized(data, p, keyname, voidid);
  return 0;
}

static inline Id
toevr(Pool *pool, struct parsedata *pd, const char *version, const char *release)
{
  return makeevr(pool, !release || (release[0] == '-' && !release[1]) ?
	version : join2(&pd->jd, version, "-", release));
}


static inline void record_share(struct parsedata *pd, Id handle, Id name, Id evr, Id arch)
{
  Repo *repo = pd->repo;
  int i = handle - repo->start;
  if (i >= pd->nshare)
    {
      pd->share_with = solv_realloc2(pd->share_with, i + 256, sizeof(*pd->share_with));
      memset(pd->share_with + pd->nshare, 0, (i + 256 - pd->nshare) * sizeof(*pd->share_with));
      pd->nshare = i + 256;
    }
  pd->share_with[i].name = name;
  pd->share_with[i].evr = evr;
  pd->share_with[i].arch = arch;
}

static void process_shares(struct parsedata *pd)
{
  Pool *pool = pd->pool;
  Repo *repo = pd->repo;
  Repodata *data = pd->data;
  int i, last_found;
  Map keyidmap;

  map_init(&keyidmap, data->nkeys);
  for (i = 1; i < data->nkeys; i++)
    {
      Id keyname = data->keys[i].name;
      if (keyname == SOLVABLE_INSTALLSIZE || keyname == SOLVABLE_DISKUSAGE || keyname == SOLVABLE_FILELIST)
	continue;
      if (keyname == SOLVABLE_MEDIADIR || keyname == SOLVABLE_MEDIAFILE || keyname == SOLVABLE_MEDIANR)
	continue;
      if (keyname == SOLVABLE_DOWNLOADSIZE || keyname == SOLVABLE_CHECKSUM)
	continue;
      if (keyname == SOLVABLE_SOURCENAME || keyname == SOLVABLE_SOURCEARCH || keyname == SOLVABLE_SOURCEEVR)
	continue;
      if (keyname == SOLVABLE_PKGID || keyname == SOLVABLE_HDRID || keyname == SOLVABLE_LEADSIGID)
	continue;
      if (keyname == SUSETAGS_SHARE_NAME || keyname == SUSETAGS_SHARE_EVR || keyname == SUSETAGS_SHARE_ARCH)
	continue;
      MAPSET(&keyidmap, i);
    }
  last_found = 0;
  for (i = 0; i < pd->nshare; i++)
    {
      unsigned int n, nn;
      Solvable *found = 0;
      if (!pd->share_with[i].name)
	continue;
      for (n = repo->start, nn = repo->start + last_found; n < repo->end; n++, nn++)
	{
	  if (nn >= repo->end)
	    nn = repo->start;
	  found = pool->solvables + nn;
	  if (found->repo == repo
	      && found->name == pd->share_with[i].name
	      && found->evr == pd->share_with[i].evr
	      && found->arch == pd->share_with[i].arch)
	    {
	      last_found = nn - repo->start;
	      break;
	    }
	}
      if (n != repo->end)
	repodata_merge_some_attrs(data, repo->start + i, repo->start + last_found, &keyidmap, 0);
    }
  pd->share_with = solv_free(pd->share_with);
  pd->nshare = 0;
  map_free(&keyidmap);
}


/*
 * parse susetags
 *
 * fp: file to read from
 * defvendor: default vendor (0 if none)
 * language: current language (0 if none)
 * flags: flags
 */

int
repo_add_susetags(Repo *repo, FILE *fp, Id defvendor, const char *language, int flags)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s;
  Offset freshens;
  int intag = 0;
  int intag_linestart = 0;
  int cummulate = 0;
  int notfound = 0;
  int indelta = 0;
  int last_found_pack = 0;
  Id first_new_pkg = 0;
  char *sp[6];
  struct parsedata pd;
  Repodata *data = 0;
  Id handle = 0;
  Hashtable joinhash = 0;
  Hashval joinhashm = 0;
  int createdpkgs = 0;

  if ((flags & (SUSETAGS_EXTEND|REPO_EXTEND_SOLVABLES)) != 0 && repo->nrepodata)
    {
      joinhash = joinhash_init(repo, &joinhashm);
    }

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  line = solv_malloc(1024);
  aline = 1024;

  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;
  pd.flags = flags;
  pd.language = language && *language ? solv_strdup(language) : 0;
  queue_init(&pd.diskusageq);

  linep = line;
  s = 0;
  freshens = 0;

  /* if this is a join setup the recorded share data */
  if (joinhash)
    {
      Repodata *sdata;
      int i;

      FOR_REPODATAS(repo, i, sdata)
	{
	  int p;
	  if (!repodata_has_keyname(sdata, SUSETAGS_SHARE_NAME))
	    continue;
	  for (p = sdata->start; p < sdata->end; p++)
	    {
	      Id name, evr, arch;
	      name = lookup_shared_id(sdata, p, SUSETAGS_SHARE_NAME, pool->solvables[p].name, sdata == data);
	      if (!name)
		continue;
	      evr = lookup_shared_id(sdata, p, SUSETAGS_SHARE_EVR, pool->solvables[p].evr, sdata == data);
	      if (!evr)
		continue;
	      arch = lookup_shared_id(sdata, p, SUSETAGS_SHARE_ARCH, pool->solvables[p].arch, sdata == data);
	      if (!arch)
		continue;
	      record_share(&pd, p, name, evr, arch);
	    }
	}
    }

  /*
   * read complete file
   *
   * collect values in 'struct parsedata pd'
   * then build .solv (and .attr) file
   */

  for (;;)
    {
      unsigned int tag;
      char line_lang[6];
      int keylen;

      if (pd.ret)
	break;
      if (linep - line + 16 > aline)              /* (re-)alloc buffer */
	{
	  aline = linep - line;
	  line = solv_realloc(line, aline + 512);
	  linep = line + aline;
	  aline += 512;
	}
      if (!fgets(linep, aline - (linep - line), fp)) /* read line */
	break;
      linep += strlen(linep);
      if (linep == line || linep[-1] != '\n')
        continue;
      pd.lineno++;
      *--linep = 0;

      if (intag)
	{
	  /* in multi-line value tags (+Key:/-Key:), check for end, cummulate */
	  int is_end = line[intag_linestart] == '-' && linep[-1] == ':' && linep - line == intag_linestart + intag + 2;
	  if (is_end && strncmp(linep - 1 - intag, line + 1, intag))
	    {
	      pool_debug(pool, SOLV_ERROR, "susetags: Nonmatching multi-line tags: %d: '%s' '%.*s'\n", pd.lineno, linep - 1 - intag, intag, line + 1);
	    }
	  if (!is_end)
	    {
	      if (cummulate)
		{
		  *linep++ = '\n';
		  intag_linestart = linep - line;
		  continue;
		}
	      intag_linestart = intag + 3;
	      linep = line + intag_linestart;
	      if (!*linep)
		continue;		/* ignore empty lines, bnc#381828 */
	    }
	  else
	    {
	      intag = 0;
	      linep = line;
	      if (!cummulate)
		continue;
	      line[intag_linestart] = 0;
	      if (line[intag_linestart - 1] == '\n')
	        line[intag_linestart - 1] = 0;		/* strip trailing newline */
	    }
	}
      else
	linep = line;

      /* ignore comments and empty lines */
      if (!*line || *line == '#')
	continue;

      /* ignore malformed lines */
      if (!(line[0] && line[1] && line[2] && line[3] && (line[4] == ':' || line[4] == '.')))
        continue;

      if (!intag && line[0] == '+' && line[1] != ':') /* start of +Key:/-Key: tag */
	{
	  char *tagend = strchr(line, ':');
	  if (!tagend || tagend - line > 100)
	    {
	      pd.ret = pool_error(pool, -1, "susetags: line %d: bad line '%s'\n", pd.lineno, line);
	      break;
	    }
	  intag = tagend - (line + 1);		/* set to tagsize */
	  cummulate = 0;
	  switch (tag_from_string(line))	/* check if accumulation is needed */
	    {
	      case CTAG('+', 'D', 'e', 's'):
	      case CTAG('+', 'E', 'u', 'l'):
	      case CTAG('+', 'I', 'n', 's'):
	      case CTAG('+', 'D', 'e', 'l'):
	      case CTAG('+', 'A', 'u', 't'):
	        cummulate = 1;
		break;
	      default:
		break;
	    }
	  line[0] = '=';			/* handle lines between +Key:/-Key: as =Key: */
	  line[intag + 2] = ' ';
	  intag_linestart = intag + 3;
	  linep = line + intag_linestart;
	  continue;
	}

      /* support language suffix */
      keylen = 3;
      line_lang[0] = 0;
      if (line[4] == '.')
        {
          char *endlang;
          endlang = strchr(line + 5, ':');
          if (endlang)
            {
	      int langsize = endlang - (line + 5);
              keylen = endlang - line - 1;
	      if (langsize > 5)
		langsize = 5;
              strncpy(line_lang, line + 5, langsize);
              line_lang[langsize] = 0;
            }
        }

      tag = tag_from_string(line);

      /* handle global tags and tags that start a new section */
      switch (tag)
        {
        case CTAG('=', 'V', 'e', 'r'):
	  /* start of a new file */
	  if (s)
	    finish_solvable(&pd, s, freshens);
	  s = 0;
	  handle = 0;
	  freshens = 0;
	  indelta = 0;
	  notfound = 0;
	  last_found_pack = 0;
	  if (createdpkgs)
	    {
	      solv_free(joinhash);
	      joinhash = joinhash_init(repo, &joinhashm);
	      createdpkgs = 0;
	    }
	  continue;

        case CTAG('=', 'L', 'a', 'n'):
	  /* define language of the following material */
	  pd.language = solv_free(pd.language);
	  memset(pd.langcache, 0, sizeof(pd.langcache));
	  if (line[6])
	    pd.language = solv_strdup(line + 6);
	  continue;

	case CTAG('=', 'P', 'k', 'g'):		/* start of a package */
	case CTAG('=', 'P', 'a', 't'):		/* start of a pattern */
	case CTAG('=', 'D', 'l', 't'):		/* start of a delta */
	  /* =Pkg: <name> <version> <release> <architecture> */
	  /* If we have an old solvable, complete it by filling in some default stuff.  */
	  if (s)
	    finish_solvable(&pd, s, freshens);
	  s = 0;
	  handle = 0;
	  freshens = 0;
	  indelta = 0;
	  notfound = 0;

	  /* define kind */
	  pd.kind = 0;
	  if (tag == CTAG('=', 'P', 'a', 't'))
	    pd.kind = "pattern";

	  /* parse nevra */
          if (split(line + 5, sp, 5) != 4)
	    {
	      pd.ret = pool_error(pool, -1, "susetags: line %d: bad line '%s'\n", pd.lineno, line);
	      break;
	    }

	  if (tag == CTAG('=', 'D', 'l', 't'))
	    {
	      /* start new delta */
	      handle = repodata_new_handle(data);
	      repodata_set_id(data, handle, DELTA_PACKAGE_NAME, pool_str2id(pool, sp[0], 1));
	      repodata_set_id(data, handle, DELTA_PACKAGE_EVR, toevr(pool, &pd, sp[1], sp[2]));
	      repodata_set_id(data, handle, DELTA_PACKAGE_ARCH, pool_str2id(pool, sp[3], 1));
	      repodata_add_flexarray(data, SOLVID_META, REPOSITORY_DELTAINFO, handle);
	      indelta = 1;
	      continue;
	    }

	  if (joinhash)
	    {
	      /* data join operation. find solvable matching name/arch/evr and
               * add data to it */
	      Id name, arch;
	      /* we don't use the create flag here as a simple pre-check for existance */
	      if (pd.kind)
		name = pool_str2id(pool, join2(&pd.jd, pd.kind, ":", sp[0]), 0);
	      else
		name = pool_str2id(pool, sp[0], 0);
	      arch = pool_str2id(pool, sp[3], 0);
	      if (name && arch)
		{
		  Id start = (flags & REPO_EXTEND_SOLVABLES) ? 0 : first_new_pkg;
		  Id evr = toevr(pool, &pd, sp[1], sp[2]);
		  if (repo->start + last_found_pack + 1 >= start && repo->start + last_found_pack + 1 < repo->end)
		    {
		      s = pool->solvables + repo->start + last_found_pack + 1;
		      if (s->repo != repo || s->name != name || s->evr != evr || s->arch != arch)
			s = 0;
		    }
		  if (!s)
		    s = joinhash_lookup(repo, joinhash, joinhashm, name, evr, arch, start);
		}
	      /* do not create new packages in EXTEND_SOLVABLES mode */
	      if (!s && (flags & REPO_EXTEND_SOLVABLES) != 0)
		{
		  notfound = 1;
		  continue;
		}
	      /* fallthrough to package creation */
	    }
	  if (!s)
	    {
	      /* normal operation. create a new solvable. */
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      if (pd.kind)
		s->name = pool_str2id(pool, join2(&pd.jd, pd.kind, ":", sp[0]), 1);
	      else
		s->name = pool_str2id(pool, sp[0], 1);
	      s->evr = toevr(pool, &pd, sp[1], sp[2]);
	      s->arch = pool_str2id(pool, sp[3], 1);
	      s->vendor = defvendor;
	      if (!first_new_pkg)
	        first_new_pkg = s - pool->solvables;
	      createdpkgs = 1;
	    }
	  handle = s - pool->solvables;
	  last_found_pack = (s - pool->solvables) - repo->start;
	  continue;

	default:
	  break;
	}

      /* handle delta tags */
      if (indelta)
	{
	  /* Example:
	    =Dlt: subversion 1.6.16 1.3.1 i586
	    =Dsq: subversion 1.6.15 4.2 i586 d57b3fc86e7a2f73796e8e35b96fa86212c910
	    =Cks: SHA1 14a8410cf741856a5d70d89dab62984dba6a1ca7
	    =Loc: 1 subversion-1.6.15_1.6.16-4.2_1.3.1.i586.delta.rpm
	    =Siz: 81558
	   */
	  switch (tag)
	    {
	    case CTAG('=', 'D', 's', 'q'):
	      {
		Id evr;
	        if (split(line + 5, sp, 5) != 5)
		  continue;
		repodata_set_id(data, handle, DELTA_SEQ_NAME, pool_str2id(pool, sp[0], 1));
		evr = toevr(pool, &pd, sp[1], sp[2]);
		repodata_set_id(data, handle, DELTA_SEQ_EVR, evr);
		/* repodata_set_id(data, handle, DELTA_SEQ_ARCH, pool_str2id(pool, sp[3], 1)); */
		repodata_set_str(data, handle, DELTA_SEQ_NUM, sp[4]);
		repodata_set_id(data, handle, DELTA_BASE_EVR, evr);
		continue;
	      }
	    case CTAG('=', 'C', 'k', 's'):
	      set_checksum(&pd, data, handle, DELTA_CHECKSUM, line + 6);
	      continue;
	    case CTAG('=', 'L', 'o', 'c'):
	      {
		int i = split(line + 6, sp, 3);
		if (i != 2 && i != 3)
		  {
		    pd.ret = pool_error(pool, -1, "susetags: line %d: bad location line '%s'\n", pd.lineno, line);
		    continue;
		  }
		repodata_set_deltalocation(data, handle, atoi(sp[0]), i == 3 ? sp[2] : 0, sp[1]);
		continue;
	      }
	    case CTAG('=', 'S', 'i', 'z'):
	      if (split(line + 6, sp, 3) == 2)
		repodata_set_num(data, handle, DELTA_DOWNLOADSIZE, strtoull(sp[0], 0, 10));
	      continue;
	    default:
	      pool_debug(pool, SOLV_WARN, "susetags: unknown line: %d: %s\n", pd.lineno, line);
	      continue;
	    }
	}

      /* we need a solvable for all other tags */
      if (!s)
	{
	  if (notfound)
	    continue;		/* did not find the solvable to extend */
#if 0
	  pool_debug(pool, SOLV_WARN, "susetags: stray line: %d: %s\n", pd.lineno, line);
#endif
          continue;
	}

      switch (tag)
        {
	  case CTAG('=', 'P', 'r', 'v'):                                        /* provides */
	    if (line[6] == '/')
	      {
		/* probably a filelist entry. stash it away for now */
		int l = strlen(line + 6) + 1;
		if (pd.nfilelist + l > pd.afilelist)
		  {
		    pd.afilelist = pd.nfilelist + l + 512;
		    pd.filelist = solv_realloc(pd.filelist, pd.afilelist);
		  }
		memcpy(pd.filelist + pd.nfilelist, line + 6, l);
		pd.nfilelist += l;
		break;
	      }
	    if (pd.nfilelist)
	      {
		int l;
		for (l = 0; l < pd.nfilelist; l += strlen(pd.filelist + l) + 1)
		  s->provides = repo_addid_dep(pd.repo, s->provides, pool_str2id(pool, pd.filelist + l, 1), 0);
		pd.nfilelist = 0;
	      }
	    s->provides = adddep(pool, &pd, s->provides, line, 0, pd.kind);
	    continue;
	  case CTAG('=', 'R', 'e', 'q'):                                        /* requires */
	    s->requires = adddep(pool, &pd, s->requires, line, -SOLVABLE_PREREQMARKER, pd.kind);
	    continue;
          case CTAG('=', 'P', 'r', 'q'):                                        /* pre-requires / packages required */
	    if (pd.kind)
	      s->requires = adddep(pool, &pd, s->requires, line, 0, 0);		/* pattern: package requires */
	    else
	      s->requires = adddep(pool, &pd, s->requires, line, SOLVABLE_PREREQMARKER, 0); /* pre-requires */
	    continue;
	  case CTAG('=', 'O', 'b', 's'):                                        /* obsoletes */
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'C', 'o', 'n'):                                        /* conflicts */
	    s->conflicts = adddep(pool, &pd, s->conflicts, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'R', 'e', 'c'):                                        /* recommends */
	    s->recommends = adddep(pool, &pd, s->recommends, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'S', 'u', 'p'):                                        /* supplements */
	    s->supplements = adddep(pool, &pd, s->supplements, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'E', 'n', 'h'):                                        /* enhances */
	    s->enhances = adddep(pool, &pd, s->enhances, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'S', 'u', 'g'):                                        /* suggests */
	    s->suggests = adddep(pool, &pd, s->suggests, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'F', 'r', 'e'):                                        /* freshens */
	    freshens = adddep(pool, &pd, freshens, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'P', 'r', 'c'):                                        /* packages recommended */
	    s->recommends = adddep(pool, &pd, s->recommends, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 's', 'g'):                                        /* packages suggested */
	    s->suggests = adddep(pool, &pd, s->suggests, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 'c', 'n'):                                        /* pattern: package conflicts */
	    s->conflicts = adddep(pool, &pd, s->conflicts, line, 0, 0);
	    continue;
	  case CTAG('=', 'P', 'o', 'b'):                                        /* pattern: package obsoletes */
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 'f', 'r'):                                        /* pattern: package freshens */
	    freshens = adddep(pool, &pd, freshens, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 's', 'p'):                                        /* pattern: package supplements */
	    s->supplements = adddep(pool, &pd, s->supplements, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 'e', 'n'):                                        /* pattern: package enhances */
	    s->enhances = adddep(pool, &pd, s->enhances, line, 0, 0);
	    continue;
          case CTAG('=', 'V', 'n', 'd'):                                        /* vendor */
            s->vendor = pool_str2id(pool, line + 6, 1);
            continue;

        /* From here it's the attribute tags.  */
          case CTAG('=', 'G', 'r', 'p'):
	    repodata_set_poolstr(data, handle, SOLVABLE_GROUP, line + 6);
	    continue;
          case CTAG('=', 'L', 'i', 'c'):
	    repodata_set_poolstr(data, handle, SOLVABLE_LICENSE, line + 6);
	    continue;
          case CTAG('=', 'L', 'o', 'c'):
	    {
	      int i = split(line + 6, sp, 3);
	      if (i != 2 && i != 3)
		{
		  pd.ret = pool_error(pool, -1, "susetags: line %d: bad location line '%s'\n", pd.lineno, line);
		  continue;
		}
	      repodata_set_location(data, handle, atoi(sp[0]), i == 3 ? sp[2] : pool_id2str(pool, s->arch), sp[1]);
	    }
	    continue;
          case CTAG('=', 'S', 'r', 'c'):
	    add_source(&pd, line + 6, s, handle);
	    continue;
          case CTAG('=', 'S', 'i', 'z'):
	    if (split(line + 6, sp, 3) == 2)
	      {
		repodata_set_num(data, handle, SOLVABLE_DOWNLOADSIZE, strtoull(sp[0], 0, 10));
		repodata_set_num(data, handle, SOLVABLE_INSTALLSIZE, strtoull(sp[1], 0, 10));
	      }
	    continue;
          case CTAG('=', 'T', 'i', 'm'):
	    {
	      unsigned int t = atoi(line + 6);
	      if (t)
		repodata_set_num(data, handle, SOLVABLE_BUILDTIME, t);
	    }
	    continue;
          case CTAG('=', 'K', 'w', 'd'):
	    repodata_add_poolstr_array(data, handle, SOLVABLE_KEYWORDS, line + 6);
	    continue;
          case CTAG('=', 'A', 'u', 't'):
	    repodata_set_str(data, handle, SOLVABLE_AUTHORS, line + 6);
	    continue;
          case CTAG('=', 'S', 'u', 'm'):
            repodata_set_str(data, handle, langtag(&pd, SOLVABLE_SUMMARY, line_lang), line + 3 + keylen);
            continue;
          case CTAG('=', 'D', 'e', 's'):
	    repodata_set_str(data, handle, langtag(&pd, SOLVABLE_DESCRIPTION, line_lang), line + 3 + keylen);
	    continue;
          case CTAG('=', 'E', 'u', 'l'):
	    repodata_set_str(data, handle, langtag(&pd, SOLVABLE_EULA, line_lang), line + 6);
	    continue;
          case CTAG('=', 'I', 'n', 's'):
	    repodata_set_str(data, handle, langtag(&pd, SOLVABLE_MESSAGEINS, line_lang), line + 6);
	    continue;
          case CTAG('=', 'D', 'e', 'l'):
	    repodata_set_str(data, handle, langtag(&pd, SOLVABLE_MESSAGEDEL, line_lang), line + 6);
	    continue;
          case CTAG('=', 'V', 'i', 's'):
	    {
	      /* Accept numbers and textual bools.  */
	      int k;
	      k = atoi(line + 6);
	      if (k || !strcasecmp(line + 6, "true"))
	        repodata_set_void(data, handle, SOLVABLE_ISVISIBLE);
	      continue;
	    }
          case CTAG('=', 'S', 'h', 'r'):
	    {
	      Id name, evr, arch;
	      if (split(line + 6, sp, 5) != 4)
		{
		  pd.ret = pool_error(pool, -1, "susetags: line %d: bad =Shr line '%s'\n", pd.lineno, line);
		  continue;
		}
	      name = pool_str2id(pool, sp[0], 1);
	      evr = toevr(pool, &pd, sp[1], sp[2]);
	      arch = pool_str2id(pool, sp[3], 1);
	      record_share(&pd, handle, name, evr, arch);
	      if ((flags & SUSETAGS_RECORD_SHARES) != 0)
		{
		  if (s->name == name)
		    repodata_set_void(data, handle, SUSETAGS_SHARE_NAME);
		  else
		    repodata_set_id(data, handle, SUSETAGS_SHARE_NAME, name);
		  if (s->evr == evr)
		    repodata_set_void(data, handle, SUSETAGS_SHARE_EVR);
		  else
		    repodata_set_id(data, handle, SUSETAGS_SHARE_EVR, evr);
		  if (s->arch == arch)
		    repodata_set_void(data, handle, SUSETAGS_SHARE_ARCH);
		  else
		    repodata_set_id(data, handle, SUSETAGS_SHARE_ARCH, arch);
		}
	      continue;
	    }
	  case CTAG('=', 'D', 'i', 'r'):
	    if (split(line + 6, sp, 6) == 5)
	      {
	        long filesz, filenum;
		Id did;

	        filesz = strtol(sp[1], 0, 0);
	        filesz += strtol(sp[2], 0, 0);
	        filenum = strtol(sp[3], 0, 0);
	        filenum += strtol(sp[4], 0, 0);
	        if (*sp[0] != '/')
		  *--sp[0] = '/'; 	/* hack: we know that there's room for a / */
	        did = repodata_str2dir(data, sp[0], 1);
	        queue_push(&pd.diskusageq, did);
	        queue_push2(&pd.diskusageq, (Id)filesz, (Id)filenum);
	      }
	    break;
	  case CTAG('=', 'C', 'a', 't'):
	    repodata_set_poolstr(data, handle, langtag(&pd, SOLVABLE_CATEGORY, line_lang), line + 3 + keylen);
	    break;
	  case CTAG('=', 'O', 'r', 'd'):
	    /* Order is a string not a number, so we can retroactively insert
	       new patterns in the middle, i.e. 1 < 15 < 2.  */
	    repodata_set_str(data, handle, SOLVABLE_ORDER, line + 6);
	    break;
	  case CTAG('=', 'I', 'c', 'o'):
	    repodata_set_str(data, handle, SOLVABLE_ICON, line + 6);
	    break;
	  case CTAG('=', 'E', 'x', 't'):
	    repodata_add_poolstr_array(data, handle, SOLVABLE_EXTENDS, join2(&pd.jd, "pattern", ":", line + 6));
	    break;
	  case CTAG('=', 'I', 'n', 'c'):
	    repodata_add_poolstr_array(data, handle, SOLVABLE_INCLUDES, join2(&pd.jd, "pattern", ":", line + 6));
	    break;
	  case CTAG('=', 'C', 'k', 's'):
	    set_checksum(&pd, data, handle, SOLVABLE_CHECKSUM, line + 6);
	    break;

	  case CTAG('=', 'F', 'l', 's'):
	    {
	      char *p, *file = line + 6;
	      Id did;

	      if (*file != '/')
	        *--file = '/';		/* hack: we know there is room */
	      p  = strrchr(file, '/');
	      /* strip trailing slashes */
	      while (p != file && !p[1])
		{
		  *p = 0;
		  p = strrchr(file, '/');
		}
	      *p++ = 0;
	      did = repodata_str2dir(data, *file ? file : "/", 1);
	      repodata_add_dirstr(data, handle, SOLVABLE_FILELIST, did, p);
	      line[5] = ' ';
	      break;
	    }
	  case CTAG('=', 'H', 'd', 'r'):
	    /* rpm header range */
	    if (split(line + 6, sp, 3) == 2)
	      {
		/* we ignore the start value */
		unsigned int end = (unsigned int)atoi(sp[1]);
		if (end)
		  repodata_set_num(data, handle, SOLVABLE_HEADEREND, end);
	      }
	    break;

	  default:
#if 0
	    pool_debug(pool, SOLV_WARN, "susetags: unknown line: %d: %s\n", pd.lineno, line);
#endif
	    break;
	}
    }

  if (s)
    finish_solvable(&pd, s, freshens);
  solv_free(pd.filelist);

  /* process shared attributes (e.g. multiple binaries built from same source) */
  if (pd.nshare)
    process_shares(&pd);

  solv_free(joinhash);
  repodata_free_dircache(data);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);

  solv_free(pd.language);
  solv_free(line);
  join_freemem(&pd.jd);
  queue_free(&pd.diskusageq);
  return pd.ret;
}
