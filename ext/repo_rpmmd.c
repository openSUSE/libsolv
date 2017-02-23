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
#define DISABLE_SPLIT
#include "tools_util.h"
#include "repo_rpmmd.h"
#include "chksum.h"
#include "solv_xmlparser.h"
#ifdef ENABLE_COMPLEX_DEPS
#include "pool_parserpmrichdep.h"
#endif
#include "repodata_diskusage.h"

enum state {
  STATE_START,

  STATE_SOLVABLE,

  STATE_NAME,
  STATE_ARCH,
  STATE_VERSION,

  /* package rpm-md */
  STATE_LOCATION,
  STATE_CHECKSUM,
  STATE_RPM_GROUP,
  STATE_RPM_LICENSE,

  /* resobject attributes */
  STATE_SUMMARY,
  STATE_DESCRIPTION,
  STATE_DISTRIBUTION,
  STATE_PACKAGER,
  STATE_URL,
  STATE_INSNOTIFY,
  STATE_DELNOTIFY,
  STATE_VENDOR,
  STATE_SIZE,
  STATE_TIME,
  STATE_DOWNLOADSIZE,
  STATE_INSTALLTIME,
  STATE_INSTALLONLY,

  /* Novell/SUSE extended attributes */
  STATE_EULA,
  STATE_KEYWORD,
  STATE_DISKUSAGE,
  STATE_DIRS,
  STATE_DIR,

  /* patch */
  STATE_ID,
  STATE_TIMESTAMP,
  STATE_AFFECTSPKG,
  STATE_REBOOTNEEDED,

  /* pattern attributes */
  STATE_CATEGORY, /* pattern and patches */
  STATE_ORDER,
  STATE_INCLUDES,
  STATE_INCLUDESENTRY,
  STATE_EXTENDS,
  STATE_EXTENDSENTRY,
  STATE_SCRIPT,
  STATE_ICON,
  STATE_USERVISIBLE,
  STATE_DEFAULT,
  STATE_INSTALL_TIME,

  /* product */
  STATE_RELNOTESURL,
  STATE_UPDATEURL,
  STATE_OPTIONALURL,
  STATE_FLAG,

  /* rpm-md dependencies inside the format tag */
  STATE_PROVIDES,
  STATE_REQUIRES,
  STATE_OBSOLETES,
  STATE_CONFLICTS,
  STATE_RECOMMENDS,
  STATE_SUPPLEMENTS,
  STATE_SUGGESTS,
  STATE_ENHANCES,
  STATE_FRESHENS,
  STATE_SOURCERPM,
  STATE_HEADERRANGE,

  STATE_PROVIDESENTRY,
  STATE_REQUIRESENTRY,
  STATE_OBSOLETESENTRY,
  STATE_CONFLICTSENTRY,
  STATE_RECOMMENDSENTRY,
  STATE_SUPPLEMENTSENTRY,
  STATE_SUGGESTSENTRY,
  STATE_ENHANCESENTRY,
  STATE_FRESHENSENTRY,

  STATE_FILE,

  STATE_CHANGELOG,

  /* general */
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  /** fake tag used to enclose multiple xml files in one **/
  { STATE_START,       "rpmmd",           STATE_START,    0 },

  /** tags for different package data, just ignore them **/
  { STATE_START,       "patterns",        STATE_START,    0 },
  { STATE_START,       "products",        STATE_START,    0 },
  { STATE_START,       "metadata",        STATE_START,    0 },
  { STATE_START,       "otherdata",       STATE_START,    0 },
  { STATE_START,       "filelists",       STATE_START,    0 },
  { STATE_START,       "diskusagedata",   STATE_START,    0 },
  { STATE_START,       "susedata",        STATE_START,    0 },

  { STATE_START,       "product",         STATE_SOLVABLE, 0 },
  { STATE_START,       "pattern",         STATE_SOLVABLE, 0 },
  { STATE_START,       "patch",           STATE_SOLVABLE, 0 },
  { STATE_START,       "package",         STATE_SOLVABLE, 0 },

  { STATE_SOLVABLE,    "format",          STATE_SOLVABLE, 0 },

  { STATE_SOLVABLE,    "name",            STATE_NAME, 1 },
  { STATE_SOLVABLE,    "arch",            STATE_ARCH, 1 },
  { STATE_SOLVABLE,    "version",         STATE_VERSION, 0 },

  /* package attributes rpm-md */
  { STATE_SOLVABLE,    "location",        STATE_LOCATION, 0 },
  { STATE_SOLVABLE,    "checksum",        STATE_CHECKSUM, 1 },

  /* resobject attributes */

  { STATE_SOLVABLE,    "summary",         STATE_SUMMARY,      1 },
  { STATE_SOLVABLE,    "description",     STATE_DESCRIPTION,  1 },
  { STATE_SOLVABLE,    "distribution",    STATE_DISTRIBUTION, 1 },
  { STATE_SOLVABLE,    "url",             STATE_URL,          1 },
  { STATE_SOLVABLE,    "packager",        STATE_PACKAGER,     1 },
  { STATE_SOLVABLE,    "vendor",          STATE_VENDOR,       1 },
  { STATE_SOLVABLE,    "size",            STATE_SIZE,         0 },
  { STATE_SOLVABLE,    "archive-size",    STATE_DOWNLOADSIZE, 1 },
  { STATE_SOLVABLE,    "install-time",    STATE_INSTALLTIME,  1 },
  { STATE_SOLVABLE,    "install-only",    STATE_INSTALLONLY,  1 },
  { STATE_SOLVABLE,    "time",            STATE_TIME,         0 },

  /* extended Novell/SUSE attributes (susedata.xml) */
  { STATE_SOLVABLE,    "eula",            STATE_EULA,         1 },
  { STATE_SOLVABLE,    "keyword",         STATE_KEYWORD,      1 },
  { STATE_SOLVABLE,    "diskusage",       STATE_DISKUSAGE,    0 },

  /* pattern attribute */
  { STATE_SOLVABLE,    "script",          STATE_SCRIPT,        1 },
  { STATE_SOLVABLE,    "icon",            STATE_ICON,          1 },
  { STATE_SOLVABLE,    "uservisible",     STATE_USERVISIBLE,   1 },
  { STATE_SOLVABLE,    "category",        STATE_CATEGORY,      1 },
  { STATE_SOLVABLE,    "order",           STATE_ORDER,         1 },
  { STATE_SOLVABLE,    "includes",        STATE_INCLUDES,      0 },
  { STATE_SOLVABLE,    "extends",         STATE_EXTENDS,       0 },
  { STATE_SOLVABLE,    "default",         STATE_DEFAULT,       1 },
  { STATE_SOLVABLE,    "install-time",    STATE_INSTALL_TIME,  1 },

  /* product attributes */
  /* note the product type is an attribute */
  { STATE_SOLVABLE,    "release-notes-url", STATE_RELNOTESURL, 1 },
  { STATE_SOLVABLE,    "update-url",      STATE_UPDATEURL,   1 },
  { STATE_SOLVABLE,    "optional-url",    STATE_OPTIONALURL, 1 },
  { STATE_SOLVABLE,    "flag",            STATE_FLAG,        1 },

  { STATE_SOLVABLE,    "rpm:vendor",      STATE_VENDOR,      1 },
  { STATE_SOLVABLE,    "rpm:group",       STATE_RPM_GROUP,   1 },
  { STATE_SOLVABLE,    "rpm:license",     STATE_RPM_LICENSE, 1 },

  /* rpm-md dependencies */
  { STATE_SOLVABLE,    "rpm:provides",    STATE_PROVIDES,     0 },
  { STATE_SOLVABLE,    "rpm:requires",    STATE_REQUIRES,     0 },
  { STATE_SOLVABLE,    "rpm:obsoletes",   STATE_OBSOLETES,    0 },
  { STATE_SOLVABLE,    "rpm:conflicts",   STATE_CONFLICTS,    0 },
  { STATE_SOLVABLE,    "rpm:recommends",  STATE_RECOMMENDS ,  0 },
  { STATE_SOLVABLE,    "rpm:supplements", STATE_SUPPLEMENTS,  0 },
  { STATE_SOLVABLE,    "rpm:suggests",    STATE_SUGGESTS,     0 },
  { STATE_SOLVABLE,    "rpm:enhances",    STATE_ENHANCES,     0 },
  { STATE_SOLVABLE,    "rpm:freshens",    STATE_FRESHENS,     0 },
  { STATE_SOLVABLE,    "rpm:sourcerpm",   STATE_SOURCERPM,    1 },
  { STATE_SOLVABLE,    "rpm:header-range", STATE_HEADERRANGE, 0 },
  { STATE_SOLVABLE,    "file",            STATE_FILE, 1 },
  { STATE_SOLVABLE,    "changelog",       STATE_CHANGELOG, 1 },

   /* extended Novell/SUSE diskusage attributes (susedata.xml) */
  { STATE_DISKUSAGE,   "dirs",            STATE_DIRS,         0 },
  { STATE_DIRS,        "dir",             STATE_DIR,          0 },

  { STATE_PROVIDES,    "rpm:entry",       STATE_PROVIDESENTRY, 0 },
  { STATE_REQUIRES,    "rpm:entry",       STATE_REQUIRESENTRY, 0 },
  { STATE_OBSOLETES,   "rpm:entry",       STATE_OBSOLETESENTRY, 0 },
  { STATE_CONFLICTS,   "rpm:entry",       STATE_CONFLICTSENTRY, 0 },
  { STATE_RECOMMENDS,  "rpm:entry",       STATE_RECOMMENDSENTRY, 0 },
  { STATE_SUPPLEMENTS, "rpm:entry",       STATE_SUPPLEMENTSENTRY, 0 },
  { STATE_SUGGESTS,    "rpm:entry",       STATE_SUGGESTSENTRY, 0 },
  { STATE_ENHANCES,    "rpm:entry",       STATE_ENHANCESENTRY, 0 },
  { STATE_FRESHENS,    "rpm:entry",       STATE_FRESHENSENTRY, 0 },

  { STATE_INCLUDES,    "item",            STATE_INCLUDESENTRY, 0 },
  { STATE_EXTENDS,     "item",            STATE_EXTENDSENTRY,  0 },

  { NUMSTATES}
};

struct parsedata {
  int ret;
  Pool *pool;
  Repo *repo;
  Repodata *data;
  char *kind;
  Solvable *solvable;
  Offset freshens;

  struct solv_xmlparser xmlp;
  struct joindata jd;
  /* temporal to store attribute tag language */
  const char *tmplang;
  Id chksumtype;
  Id handle;
  Queue diskusageq;
  const char *language;			/* default language */
  Id langcache[ID_NUM_INTERNAL];	/* cache for the default language */

  Id lastdir;
  char *lastdirstr;
  int lastdirstrl;

  Id changelog_handle;

  int extending;			/* are we extending an existing solvable? */
  int first;				/* first solvable we added */
  int cshash_filled;			/* hash is filled with data */

  Hashtable cshash;			/* checksum hash -> offset into csdata */
  Hashval cshashm;			/* hash mask */
  int ncshash;				/* entries used */
  unsigned char *csdata;		/* [len, checksum, id] */
  int ncsdata;				/* used bytes */
};

static Id
langtag(struct parsedata *pd, Id tag, const char *language)
{
  if (language)
    {
      if (!language[0] || !strcmp(language, "en"))
	return tag;
      return pool_id2langid(pd->pool, tag, language, 1);
    }
  if (!pd->language)
    return tag;
  if (tag >= ID_NUM_INTERNAL)
    return pool_id2langid(pd->pool, tag, pd->language, 1);
  if (!pd->langcache[tag])
    pd->langcache[tag] = pool_id2langid(pd->pool, tag, pd->language, 1);
  return pd->langcache[tag];
}

/*
 * makeevr_atts
 * parse 'epoch', 'ver' and 'rel', return evr Id
 *
 */

static Id
makeevr_atts(Pool *pool, struct parsedata *pd, const char **atts)
{
  const char *e, *v, *r, *v2;
  char *c, *space;
  int l;

  e = v = r = 0;
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, "epoch"))
	e = atts[1];
      else if (!strcmp(*atts, "ver"))
	v = atts[1];
      else if (!strcmp(*atts, "rel"))
	r = atts[1];
    }
  if (e && (!*e || !strcmp(e, "0")))
    e = 0;
  if (v && !e)
    {
      for (v2 = v; *v2 >= '0' && *v2 <= '9'; v2++)
        ;
      if (v2 > v && *v2 == ':')
	e = "0";
    }
  l = 1;
  if (e)
    l += strlen(e) + 1;
  if (v)
    l += strlen(v);
  if (r)
    l += strlen(r) + 1;
  c = space = solv_xmlparser_contentspace(&pd->xmlp, l);
  if (e)
    {
      strcpy(c, e);
      c += strlen(c);
      *c++ = ':';
    }
  if (v)
    {
      strcpy(c, v);
      c += strlen(c);
    }
  if (r)
    {
      *c++ = '-';
      strcpy(c, r);
      c += strlen(c);
    }
  *c = 0;
  if (!*space)
    return 0;
#if 0
  fprintf(stderr, "evr: %s\n", space);
#endif
  return pool_str2id(pool, space, 1);
}


/*
 * dependency relations
 */

static char *flagtab[] = {
  "GT",
  "EQ",
  "GE",
  "LT",
  "NE",
  "LE"
};


/*
 * adddep
 * parse attributes to reldep Id
 *
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, const char **atts, int isreq)
{
  Id id, marker;
  const char *n, *f, *k;
  const char **a;

  n = f = k = 0;
  marker = isreq ? -SOLVABLE_PREREQMARKER : 0;
  for (a = atts; *a; a += 2)
    {
      if (!strcmp(*a, "name"))
	n = a[1];
      else if (!strcmp(*a, "flags"))
	f = a[1];
      else if (!strcmp(*a, "kind"))
	k = a[1];
      else if (isreq && !strcmp(*a, "pre") && a[1][0] == '1')
	marker = SOLVABLE_PREREQMARKER;
    }
  if (!n)
    return olddeps;
  if (k && !strcmp(k, "package"))
    k = 0;
  if (k)
    {
      int l = strlen(k) + 1 + strlen(n) + 1;
      char *space = solv_xmlparser_contentspace(&pd->xmlp, l);
      sprintf(space, "%s:%s", k, n);
      id = pool_str2id(pool, space, 1);
    }
#ifdef ENABLE_COMPLEX_DEPS
  else if (!f && n[0] == '(')
    {
      id = pool_parserpmrichdep(pool, n);
      if (!id)
	return olddeps;
    }
#endif
  else
    id = pool_str2id(pool, (char *)n, 1);
  if (f)
    {
      Id evr = makeevr_atts(pool, pd, atts);
      int flags;
      for (flags = 0; flags < 6; flags++)
	if (!strcmp(f, flagtab[flags]))
	  break;
      flags = flags < 6 ? flags + 1 : 0;
      id = pool_rel2id(pool, id, evr, flags, 1);
    }
#if 0
  fprintf(stderr, "new dep %s\n", pool_dep2str(pool, id));
#endif
  return repo_addid_dep(pd->repo, olddeps, id, marker);
}


/*
 * set_description_author
 *
 */
static void
set_description_author(Repodata *data, Id handle, char *str, struct parsedata *pd)
{
  char *aut, *p;

  if (!str || !*str)
    return;
  for (aut = str; (aut = strchr(aut, '\n')) != 0; aut++)
    if (!strncmp(aut, "\nAuthors:\n--------\n", 19))
      break;
  if (aut)
    {
      /* oh my, found SUSE special author section */
      int l = aut - str;
      str[l] = 0;
      while (l > 0 && str[l - 1] == '\n')
	str[--l] = 0;
      if (l)
	repodata_set_str(data, handle, langtag(pd, SOLVABLE_DESCRIPTION, pd->tmplang), str);
      p = aut + 19;
      aut = str;        /* copy over */
      while (*p == ' ' || *p == '\n')
	p++;
      while (*p)
	{
	  if (*p == '\n')
	    {
	      *aut++ = *p++;
	      while (*p == ' ')
		p++;
	      continue;
	    }
	  *aut++ = *p++;
	}
      while (aut != str && aut[-1] == '\n')
	aut--;
      *aut = 0;
      if (*str)
	repodata_set_str(data, handle, SOLVABLE_AUTHORS, str);
    }
  else if (*str)
    repodata_set_str(data, handle, langtag(pd, SOLVABLE_DESCRIPTION, pd->tmplang), str);
}


/*-----------------------------------------------*/
/* checksum hash functions
 *
 * used to look up a solvable with the checksum for solvable extension purposes.
 *
 */

static void
init_cshash(struct parsedata *pd)
{
}

static void
free_cshash(struct parsedata *pd)
{
  pd->cshash = solv_free(pd->cshash);
  pd->ncshash = 0;
  pd->cshashm = 0;
  pd->csdata = solv_free(pd->csdata);
  pd->ncsdata = 0;
}

static inline Hashval
hashkey(const unsigned char *key, int keyl)
{
  return key[0] << 24 | key[1] << 16 | key[2] << 8 | key[3];
}

static void
rebuild_cshash(struct parsedata *pd)
{
  Hashval h, hh, hm;
  Hashtable ht;
  unsigned char *d, *de;

  hm = pd->cshashm;
#if 0
  fprintf(stderr, "rebuild cshash with mask 0x%x\n", hm);
#endif
  solv_free(pd->cshash);
  ht = pd->cshash = (Hashtable)solv_calloc(hm + 1, sizeof(Id));
  d = pd->csdata;
  de = d + pd->ncsdata;
  while (d != de)
    {
      h = hashkey(d + 1, d[0] + 1) & hm;
      hh = HASHCHAIN_START;
      while (ht[h])
	h = HASHCHAIN_NEXT(h, hh, hm);
      ht[h] = d + 1 - pd->csdata;
      d += 2 + d[0] + sizeof(Id);
    }
}

static void
put_in_cshash(struct parsedata *pd, const unsigned char *key, int keyl, Id id)
{
  Hashtable ht;
  Hashval h, hh, hm;
  unsigned char *d;

  if (keyl < 4 || keyl > 256)
    return;
  ht = pd->cshash;
  hm = pd->cshashm;
  h = hashkey(key, keyl) & hm;
  hh = HASHCHAIN_START;
  if (ht)
    {
      while (ht[h])
	{
	  unsigned char *d = pd->csdata + ht[h];
	  if (d[-1] == keyl - 1 && !memcmp(key, d, keyl))
	    return;		/* XXX: first id wins... */
	  h = HASHCHAIN_NEXT(h, hh, hm);
	}
    }
  /* a new entry. put in csdata */
  pd->csdata = solv_extend(pd->csdata, pd->ncsdata, 1 + keyl + sizeof(Id), 1, 4095);
  d = pd->csdata + pd->ncsdata;
  d[0] = keyl - 1;
  memcpy(d + 1, key, keyl);
  memcpy(d + 1 + keyl, &id, sizeof(Id));
  pd->ncsdata += 1 + keyl + sizeof(Id);
  if ((Hashval)++pd->ncshash * 2 > hm)
    {
      pd->cshashm = pd->cshashm ? (2 * pd->cshashm + 1) : 4095;
      rebuild_cshash(pd);
    }
  else
    ht[h] = pd->ncsdata - (keyl + sizeof(Id));
}

static Id
lookup_cshash(struct parsedata *pd, const unsigned char *key, int keyl)
{
  Hashtable ht;
  Hashval h, hh, hm;

  if (keyl < 4 || keyl > 256)
    return 0;
  ht = pd->cshash;
  if (!ht)
    return 0;
  hm = pd->cshashm;
  h = hashkey(key, keyl) & hm;
  hh = HASHCHAIN_START;
  while (ht[h])
    {
      unsigned char *d = pd->csdata + ht[h];
      if (d[-1] == keyl - 1 && !memcmp(key, d, keyl))
	{
	  Id id;
	  memcpy(&id, d + keyl, sizeof(Id));
	  return id;
	}
      h = HASHCHAIN_NEXT(h, hh, hm);
    }
  return 0;
}

static void
fill_cshash_from_repo(struct parsedata *pd)
{
  Dataiterator di;
  /* setup join data */
  dataiterator_init(&di, pd->pool, pd->repo, 0, SOLVABLE_CHECKSUM, 0, 0);
  while (dataiterator_step(&di))
    put_in_cshash(pd, (const unsigned char *)di.kv.str, solv_chksum_len(di.key->type), di.solvid);
  dataiterator_free(&di);
}

static void
fill_cshash_from_new_solvables(struct parsedata *pd)
{
  Pool *pool = pd->pool;
  Id cstype = 0;
  unsigned const char *cs;
  int i;

  for (i = pd->first; i < pool->nsolvables; i++)
    {
      if (pool->solvables[i].repo != pd->repo)
	continue;
      cs = repodata_lookup_bin_checksum_uninternalized(pd->data, i, SOLVABLE_CHECKSUM, &cstype);
      if (cs)
	put_in_cshash(pd, cs, solv_chksum_len(cstype), i);
    }
}

/*-----------------------------------------------*/
/* XML callbacks */

/*
 * startElement
 */

static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  Id handle = pd->handle;
  const char *str;
  const char *pkgid;

  if (!s && state != STATE_SOLVABLE)
    return;

  switch(state)
    {
    case STATE_SOLVABLE:
      pd->kind = 0;
      if (name[2] == 't' && name[3] == 't')
        pd->kind = "pattern";
      else if (name[1] == 'r')
        pd->kind = "product";
      else if (name[2] == 't' && name[3] == 'c')
        pd->kind = "patch";

      /* to support extension metadata files like others.xml which
         have the following structure:

         <otherdata xmlns="http://linux.duke.edu/metadata/other"
                    packages="101">
           <package pkgid="b78f8664cd90efe42e09a345e272997ef1b53c18"
                    name="zaptel-kmp-default"
                    arch="i586"><version epoch="0"
                    ver="1.2.10_2.6.22_rc4_git6_2" rel="70"/>
              ...

         we need to check if the pkgid is there and if it matches
         an already seen package, that means we don't need to create
         a new solvable but just append the attributes to the existing
         one.
      */
      pd->extending = 0;
      if ((pkgid = solv_xmlparser_find_attr("pkgid", atts)) != NULL)
        {
	  unsigned char chk[256];
	  int l;
	  const char *str = pkgid;
	  if (!pd->cshash_filled)
	    {
	      pd->cshash_filled = 1;
	      fill_cshash_from_new_solvables(pd);
	    }
	  handle = 0;
	  /* convert into bin checksum */
	  l = solv_hex2bin(&str, chk, sizeof(chk));
          /* look at the checksum cache */
	  if (l >= 4 && !pkgid[2 * l])
	    handle = lookup_cshash(pd, chk, l);
#if 0
	  fprintf(stderr, "Lookup %s -> %d\n", pkgid, handle);
#endif
	  if (!handle)
	    {
              pool_debug(pool, SOLV_WARN, "the repository specifies extra information about package with checksum '%s', which does not exist in the repository.\n", pkgid);
	      pd->handle = 0;
	      pd->solvable = 0;
	      break;
	    }
	  pd->extending = 1;
        }
      else
        {
          /* this is a new package */
	  handle = repo_add_solvable(pd->repo);
	  if (!pd->first)
	    pd->first = handle;
          pd->freshens = 0;
        }
      pd->handle = handle;
      pd->solvable = pool_id2solvable(pool, handle);
      if (pd->kind && pd->kind[1] == 'r')
	{
	  /* products can have a type */
	  const char *type = solv_xmlparser_find_attr("type", atts);
	  if (type && *type)
	    repodata_set_str(pd->data, handle, PRODUCT_TYPE, type);
	}
#if 0
      fprintf(stderr, "package #%d\n", pd->solvable - pool->solvables);
#endif

      break;
    case STATE_VERSION:
      if (pd->extending && s->evr)
	break;		/* ignore version tag repetition in extend data */
      s->evr = makeevr_atts(pool, pd, atts);
      break;
    case STATE_PROVIDES:
      s->provides = 0;
      break;
    case STATE_PROVIDESENTRY:
      s->provides = adddep(pool, pd, s->provides, atts, 0);
      break;
    case STATE_REQUIRES:
      s->requires = 0;
      break;
    case STATE_REQUIRESENTRY:
      s->requires = adddep(pool, pd, s->requires, atts, 1);
      break;
    case STATE_OBSOLETES:
      s->obsoletes = 0;
      break;
    case STATE_OBSOLETESENTRY:
      s->obsoletes = adddep(pool, pd, s->obsoletes, atts, 0);
      break;
    case STATE_CONFLICTS:
      s->conflicts = 0;
      break;
    case STATE_CONFLICTSENTRY:
      s->conflicts = adddep(pool, pd, s->conflicts, atts, 0);
      break;
    case STATE_RECOMMENDS:
      s->recommends = 0;
      break;
    case STATE_RECOMMENDSENTRY:
      s->recommends = adddep(pool, pd, s->recommends, atts, 0);
      break;
    case STATE_SUPPLEMENTS:
      s->supplements= 0;
      break;
    case STATE_SUPPLEMENTSENTRY:
      s->supplements = adddep(pool, pd, s->supplements, atts, 0);
      break;
    case STATE_SUGGESTS:
      s->suggests = 0;
      break;
    case STATE_SUGGESTSENTRY:
      s->suggests = adddep(pool, pd, s->suggests, atts, 0);
      break;
    case STATE_ENHANCES:
      s->enhances = 0;
      break;
    case STATE_ENHANCESENTRY:
      s->enhances = adddep(pool, pd, s->enhances, atts, 0);
      break;
    case STATE_FRESHENS:
      pd->freshens = 0;
      break;
    case STATE_FRESHENSENTRY:
      pd->freshens = adddep(pool, pd, pd->freshens, atts, 0);
      break;
    case STATE_EULA:
    case STATE_SUMMARY:
    case STATE_CATEGORY:
    case STATE_DESCRIPTION:
      pd->tmplang = join_dup(&pd->jd, solv_xmlparser_find_attr("lang", atts));
      break;
    case STATE_USERVISIBLE:
      repodata_set_void(pd->data, handle, SOLVABLE_ISVISIBLE);
      break;
    case STATE_INCLUDESENTRY:
      str = solv_xmlparser_find_attr("pattern", atts);
      if (str)
	repodata_add_poolstr_array(pd->data, handle, SOLVABLE_INCLUDES, join2(&pd->jd, "pattern", ":", str));
      break;
    case STATE_EXTENDSENTRY:
      str = solv_xmlparser_find_attr("pattern", atts);
      if (str)
	repodata_add_poolstr_array(pd->data, handle, SOLVABLE_EXTENDS, join2(&pd->jd, "pattern", ":", str));
      break;
    case STATE_LOCATION:
      str = solv_xmlparser_find_attr("href", atts);
      if (str)
	{
	  int medianr = 0;
	  const char *base = solv_xmlparser_find_attr("xml:base", atts);
	  if (base  && !strncmp(base, "media:", 6))
	    {
	      /* check for the media number in the fragment */
	      int l = strlen(base);
	      while (l && base[l - 1] >= '0' && base[l - 1] <= '9')
		l--;
	      if (l && base[l - 1] == '#' && base[l])
		medianr = atoi(base + l);
	    }
	  repodata_set_location(pd->data, handle, medianr, 0, str);
	  if (base)
	    repodata_set_poolstr(pd->data, handle, SOLVABLE_MEDIABASE, base);
	}
      break;
    case STATE_CHECKSUM:
      str = solv_xmlparser_find_attr("type", atts);
      pd->chksumtype = str && *str ? solv_chksum_str2type(str) : 0;
      if (!pd->chksumtype)
	pd->ret = pool_error(pool, -1, "line %d: unknown checksum type: %s", solv_xmlparser_lineno(xmlp), str ? str : "NULL");
      break;
    case STATE_TIME:
      {
        unsigned int t;
        str = solv_xmlparser_find_attr("build", atts);
        if (str && (t = atoi(str)) != 0)
          repodata_set_num(pd->data, handle, SOLVABLE_BUILDTIME, t);
	break;
      }
    case STATE_SIZE:
      if ((str = solv_xmlparser_find_attr("installed", atts)) != 0)
	repodata_set_num(pd->data, handle, SOLVABLE_INSTALLSIZE, strtoull(str, 0, 10));
      if ((str = solv_xmlparser_find_attr("package", atts)) != 0)
	repodata_set_num(pd->data, handle, SOLVABLE_DOWNLOADSIZE, strtoull(str, 0, 10));
      break;
    case STATE_HEADERRANGE:
      {
        unsigned int end;
        str = solv_xmlparser_find_attr("end", atts);
	if (str && (end = atoi(str)) != 0)
	  repodata_set_num(pd->data, handle, SOLVABLE_HEADEREND, end);
	break;
      }
      /*
        <diskusage>
          <dirs>
            <dir name="/" size="56" count="11"/>
            <dir name="usr/" size="56" count="11"/>
            <dir name="usr/bin/" size="38" count="10"/>
            <dir name="usr/share/" size="18" count="1"/>
            <dir name="usr/share/doc/" size="18" count="1"/>
          </dirs>
        </diskusage>
      */
    case STATE_DISKUSAGE:
      {
        /* Really, do nothing, wait for <dir> tag */
        break;
      }
    case STATE_DIR:
      {
        long filesz = 0, filenum = 0;
        Id did;

        if ((str = solv_xmlparser_find_attr("name", atts)) == 0)
	  {
	    pd->ret = pool_error(pool, -1, "<dir .../> tag without 'name' attribute");
            break;
	  }
	if (*str != '/')
	  {
	    if (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC)
	      str = "/usr/src";
	    else
	      {
		int l = strlen(str) + 2;
		char *space = solv_xmlparser_contentspace(xmlp, l);
		space[0] = '/';
		memcpy(space + 1, str, l - 1);
		str = space;
	    }
	  }
        did = repodata_str2dir(pd->data, str, 1);
        if ((str = solv_xmlparser_find_attr("size", atts)) != 0)
          filesz = strtol(str, 0, 0);
        if ((str = solv_xmlparser_find_attr("count", atts)) != 0)
          filenum = strtol(str, 0, 0);
        if (filesz || filenum)
          {
            queue_push(&pd->diskusageq, did);
            queue_push2(&pd->diskusageq, filesz, filenum);
          }
        break;
      }
    case STATE_CHANGELOG:
      pd->changelog_handle = repodata_new_handle(pd->data);
      if ((str = solv_xmlparser_find_attr("date", atts)) != 0)
	repodata_set_num(pd->data, pd->changelog_handle, SOLVABLE_CHANGELOG_TIME, strtoull(str, 0, 10));
      if ((str = solv_xmlparser_find_attr("author", atts)) != 0)
	repodata_set_str(pd->data, pd->changelog_handle, SOLVABLE_CHANGELOG_AUTHOR, str);
      break;
    default:
      break;
    }
}


/*
 * endElement
 */

static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  Repo *repo = pd->repo;
  Id handle = pd->handle;
  Id id;
  char *p;

  if (!s)
    return;

  switch (state)
    {
    case STATE_SOLVABLE:
      if (pd->extending)
	{
	  pd->solvable = 0;
	  break;
	}
      if (pd->kind && !s->name) /* add namespace in case of NULL name */
        s->name = pool_str2id(pool, join2(&pd->jd, pd->kind, ":", 0), 1);
      if (!s->arch)
        s->arch = ARCH_NOARCH;
      if (!s->evr)
        s->evr = ID_EMPTY;	/* some patterns have this */
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      repo_rewrite_suse_deps(s, pd->freshens);
      pd->freshens = 0;
      pd->kind = 0;
      pd->solvable = 0;
      break;
    case STATE_NAME:
      if (pd->kind)
        s->name = pool_str2id(pool, join2(&pd->jd, pd->kind, ":", content), 1);
      else
        s->name = pool_str2id(pool, content, 1);
      break;
    case STATE_ARCH:
      s->arch = pool_str2id(pool, content, 1);
      break;
    case STATE_VENDOR:
      s->vendor = pool_str2id(pool, content, 1);
      break;
    case STATE_RPM_GROUP:
      repodata_set_poolstr(pd->data, handle, SOLVABLE_GROUP, content);
      break;
    case STATE_RPM_LICENSE:
      repodata_set_poolstr(pd->data, handle, SOLVABLE_LICENSE, content);
      break;
    case STATE_CHECKSUM:
      {
	unsigned char chk[256];
	int l = solv_chksum_len(pd->chksumtype);
	const char *str = content;
	if (!l || l > sizeof(chk))
	  break;
	if (solv_hex2bin(&str, chk, l) != l || content[2 * l])
          {
	    pd->ret = pool_error(pool, -1, "line %u: invalid %s checksum", solv_xmlparser_lineno(xmlp), solv_chksum_type2str(pd->chksumtype));
	    break;
          }
        repodata_set_bin_checksum(pd->data, handle, SOLVABLE_CHECKSUM, pd->chksumtype, chk);
	/* we save the checksum to solvable id relationship for extending metadata */
	if (pd->cshash_filled)
	  put_in_cshash(pd, chk, l, s - pool->solvables);
        break;
      }
    case STATE_FILE:
      if ((p = strrchr(content, '/')) != 0)
	{
	  *p++ = 0;
	  if (pd->lastdir && !strcmp(pd->lastdirstr, content))
	    {
	      id = pd->lastdir;
	    }
	  else
	    {
	      int l = p - content;
	      if (l + 1 > pd->lastdirstrl)	/* + 1 for the possible leading / we need to insert */
		{
		  pd->lastdirstrl = l + 128;
		  pd->lastdirstr = solv_realloc(pd->lastdirstr, pd->lastdirstrl);
		}
	      if (content[0] != '/')
		{
		  pd->lastdirstr[0] = '/';
		  memcpy(pd->lastdirstr + 1, content, l);
	          id = repodata_str2dir(pd->data, pd->lastdirstr, 1);
		}
	      else
	        id = repodata_str2dir(pd->data, content, 1);
	      pd->lastdir = id;
	      memcpy(pd->lastdirstr, content, l);
	    }
	}
      else
	{
	  p = content;
	  id = repodata_str2dir(pd->data, "/", 1);
	}
      repodata_add_dirstr(pd->data, handle, SOLVABLE_FILELIST, id, p);
      break;
    case STATE_SUMMARY:
      repodata_set_str(pd->data, handle, langtag(pd, SOLVABLE_SUMMARY, pd->tmplang), content);
      break;
    case STATE_DESCRIPTION:
      set_description_author(pd->data, handle, content, pd);
      break;
    case STATE_CATEGORY:
      repodata_set_str(pd->data, handle, langtag(pd, SOLVABLE_CATEGORY, pd->tmplang), content);
      break;
    case STATE_DISTRIBUTION:
        repodata_set_poolstr(pd->data, handle, SOLVABLE_DISTRIBUTION, content);
        break;
    case STATE_URL:
      if (*content)
	repodata_set_str(pd->data, handle, SOLVABLE_URL, content);
      break;
    case STATE_PACKAGER:
      if (*content)
	repodata_set_poolstr(pd->data, handle, SOLVABLE_PACKAGER, content);
      break;
    case STATE_SOURCERPM:
      if (*content)
	repodata_set_sourcepkg(pd->data, handle, content);
      break;
    case STATE_RELNOTESURL:
      if (*content)
        {
          repodata_add_poolstr_array(pd->data, handle, PRODUCT_URL, content);
          repodata_add_idarray(pd->data, handle, PRODUCT_URL_TYPE, pool_str2id(pool, "releasenotes", 1));
        }
      break;
    case STATE_UPDATEURL:
      if (*content)
        {
          repodata_add_poolstr_array(pd->data, handle, PRODUCT_URL, content);
          repodata_add_idarray(pd->data, handle, PRODUCT_URL_TYPE, pool_str2id(pool, "update", 1));
        }
      break;
    case STATE_OPTIONALURL:
      if (*content)
        {
          repodata_add_poolstr_array(pd->data, handle, PRODUCT_URL, content);
          repodata_add_idarray(pd->data, handle, PRODUCT_URL_TYPE, pool_str2id(pool, "optional", 1));
        }
      break;
    case STATE_FLAG:
      if (*content)
        repodata_add_poolstr_array(pd->data, handle, PRODUCT_FLAGS, content);
      break;
    case STATE_EULA:
      if (*content)
	repodata_set_str(pd->data, handle, langtag(pd, SOLVABLE_EULA, pd->tmplang), content);
      break;
    case STATE_KEYWORD:
      if (*content)
        repodata_add_poolstr_array(pd->data, handle, SOLVABLE_KEYWORDS, content);
      break;
    case STATE_DISKUSAGE:
      if (pd->diskusageq.count)
        repodata_add_diskusage(pd->data, handle, &pd->diskusageq);
      break;
    case STATE_ORDER:
      if (*content)
        repodata_set_str(pd->data, handle, SOLVABLE_ORDER, content);
      break;
    case STATE_CHANGELOG:
      repodata_set_str(pd->data, pd->changelog_handle, SOLVABLE_CHANGELOG_TEXT, content);
      repodata_add_flexarray(pd->data, handle, SOLVABLE_CHANGELOG, pd->changelog_handle);
      pd->changelog_handle = 0;
      break;
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pd->ret = pool_error(pd->pool, -1, "repo_rpmmd: %s at line %u:%u", errstr, line, column);
}


/*-----------------------------------------------*/

/*
 * repo_add_rpmmd
 * parse rpm-md metadata (primary, others)
 *
 */

int
repo_add_rpmmd(Repo *repo, FILE *fp, const char *language, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  Repodata *data;
  unsigned int now;

  now = solv_timems(0);
  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;

  pd.kind = 0;
  pd.language = language && *language && strcmp(language, "en") != 0 ? language : 0;
  queue_init(&pd.diskusageq);

  init_cshash(&pd);
  if ((flags & REPO_EXTEND_SOLVABLES) != 0)
    {
      /* setup join data */
      pd.cshash_filled = 1;
      fill_cshash_from_repo(&pd);
    }

  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);

  solv_free(pd.lastdirstr);
  join_freemem(&pd.jd);
  free_cshash(&pd);
  repodata_free_dircache(data);
  queue_free(&pd.diskusageq);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo_add_rpmmd took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo size: %d solvables\n", repo->nsolvables);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo memory used: %d K incore, %d K idarray\n", repodata_memused(data)/1024, repo->idarraysize / (int)(1024/sizeof(Id)));
  return pd.ret;
}
