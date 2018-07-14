/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * repo_helix.c
 *
 * Parse 'helix' XML representation
 * and create 'repo'
 *
 * A bit of history: "Helix Code" was the name of the company that
 * wrote Red Carpet. The company was later renamed to Ximian.
 * The Red Carpet solver was merged into the ZYPP project, the
 * library used both by ZENworks and YaST for package management.
 * Red Carpet came with solver testcases in its own repository
 * format, the 'helix' format.
 *
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "queue.h"
#include "solv_xmlparser.h"
#include "repo_helix.h"
#include "evr.h"


/* XML parser states */

enum state {
  STATE_START,
  STATE_CHANNEL,
  STATE_SUBCHANNEL,
  STATE_PACKAGE,
  STATE_NAME,
  STATE_VENDOR,
  STATE_BUILDTIME,
  STATE_HISTORY,
  STATE_UPDATE,
  STATE_EPOCH,
  STATE_VERSION,
  STATE_RELEASE,
  STATE_ARCH,
  STATE_PROVIDES,
  STATE_PROVIDESENTRY,
  STATE_REQUIRES,
  STATE_REQUIRESENTRY,
  STATE_PREREQUIRES,
  STATE_PREREQUIRESENTRY,
  STATE_OBSOLETES,
  STATE_OBSOLETESENTRY,
  STATE_CONFLICTS,
  STATE_CONFLICTSENTRY,
  STATE_RECOMMENDS,
  STATE_RECOMMENDSENTRY,
  STATE_SUPPLEMENTS,
  STATE_SUPPLEMENTSENTRY,
  STATE_SUGGESTS,
  STATE_SUGGESTSENTRY,
  STATE_ENHANCES,
  STATE_ENHANCESENTRY,
  STATE_FRESHENS,
  STATE_FRESHENSENTRY,

  STATE_SELECTTION,
  STATE_PATTERN,
  STATE_ATOM,
  STATE_PATCH,
  STATE_PRODUCT,

  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START,       "channel",         STATE_CHANNEL, 0 },
  { STATE_CHANNEL,     "subchannel",      STATE_SUBCHANNEL, 0 },
  { STATE_SUBCHANNEL,  "package",         STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "srcpackage",      STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "selection",       STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "pattern",         STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "atom",            STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "patch",           STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "product",         STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "application",     STATE_PACKAGE, 0 },
  { STATE_PACKAGE,     "name",            STATE_NAME, 1 },
  { STATE_PACKAGE,     "vendor",          STATE_VENDOR, 1 },
  { STATE_PACKAGE,     "buildtime",       STATE_BUILDTIME, 1 },
  { STATE_PACKAGE,     "epoch",           STATE_EPOCH, 1 },
  { STATE_PACKAGE,     "version",         STATE_VERSION, 1 },
  { STATE_PACKAGE,     "release",         STATE_RELEASE, 1 },
  { STATE_PACKAGE,     "arch",            STATE_ARCH, 1 },
  { STATE_PACKAGE,     "history",         STATE_HISTORY, 0 },
  { STATE_PACKAGE,     "provides",        STATE_PROVIDES, 0 },
  { STATE_PACKAGE,     "requires",        STATE_REQUIRES, 0 },
  { STATE_PACKAGE,     "prerequires",     STATE_PREREQUIRES, 0 },
  { STATE_PACKAGE,     "obsoletes",       STATE_OBSOLETES , 0 },
  { STATE_PACKAGE,     "conflicts",       STATE_CONFLICTS , 0 },
  { STATE_PACKAGE,     "recommends" ,     STATE_RECOMMENDS , 0 },
  { STATE_PACKAGE,     "supplements",     STATE_SUPPLEMENTS, 0 },
  { STATE_PACKAGE,     "suggests",        STATE_SUGGESTS, 0 },
  { STATE_PACKAGE,     "enhances",        STATE_ENHANCES, 0 },
  { STATE_PACKAGE,     "freshens",        STATE_FRESHENS, 0 },
  { STATE_PACKAGE,     "deps",            STATE_PACKAGE, 0 },	/* ignore deps element */

  { STATE_HISTORY,     "update",          STATE_UPDATE, 0 },
  { STATE_UPDATE,      "epoch",           STATE_EPOCH, 1 },
  { STATE_UPDATE,      "version",         STATE_VERSION, 1 },
  { STATE_UPDATE,      "release",         STATE_RELEASE, 1 },
  { STATE_UPDATE,      "arch",            STATE_ARCH, 1 },

  { STATE_PROVIDES,    "dep",             STATE_PROVIDESENTRY, 0 },
  { STATE_REQUIRES,    "dep",             STATE_REQUIRESENTRY, 0 },
  { STATE_PREREQUIRES, "dep",             STATE_PREREQUIRESENTRY, 0 },
  { STATE_OBSOLETES,   "dep",             STATE_OBSOLETESENTRY, 0 },
  { STATE_CONFLICTS,   "dep",             STATE_CONFLICTSENTRY, 0 },
  { STATE_RECOMMENDS,  "dep",             STATE_RECOMMENDSENTRY, 0 },
  { STATE_SUPPLEMENTS, "dep",             STATE_SUPPLEMENTSENTRY, 0 },
  { STATE_SUGGESTS,    "dep",             STATE_SUGGESTSENTRY, 0 },
  { STATE_ENHANCES,    "dep",             STATE_ENHANCESENTRY, 0 },
  { STATE_FRESHENS,    "dep",             STATE_FRESHENSENTRY, 0 },
  { NUMSTATES }

};

/*
 * parser data
 */

struct parsedata {
  int ret;
  /* repo data */
  Pool *pool;		/* current pool */
  Repo *repo;		/* current repo */
  Repodata *data;       /* current repo data */
  Solvable *solvable;	/* current solvable */
  Offset freshens;	/* current freshens vector */

  /* package data */
  int  srcpackage;	/* is srcpackage element */
  int  epoch;		/* epoch (as offset into evrspace) */
  int  version;		/* version (as offset into evrspace) */
  int  release;		/* release (as offset into evrspace) */
  char *evrspace;	/* buffer for evr */
  int  aevrspace;	/* actual buffer space */
  int  levrspace;	/* actual evr length */
  char *kind;

  struct solv_xmlparser xmlp;
};


/*------------------------------------------------------------------*/
/* E:V-R handling */

/* create Id from epoch:version-release */

static Id
evr2id(Pool *pool, struct parsedata *pd, const char *e, const char *v, const char *r)
{
  char *c, *space;
  int l;

  /* treat explitcit 0 as NULL */
  if (e && (!*e || !strcmp(e, "0")))
    e = 0;

  if (v && !e)
    {
      const char *v2;
      /* scan version for ":" */
      for (v2 = v; *v2 >= '0' && *v2 <= '9'; v2++)	/* skip leading digits */
        ;
      /* if version contains ":", set epoch to "0" */
      if (v2 > v && *v2 == ':')
	e = "0";
    }

  /* compute length of Id string */
  l = 1;  /* for the \0 */
  if (e)
    l += strlen(e) + 1;  /* e: */
  if (v)
    l += strlen(v);      /* v */
  if (r)
    l += strlen(r) + 1;  /* -r */

  /* get content space */
  c = space = solv_xmlparser_contentspace(&pd->xmlp, l);

  /* copy e-v-r */
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
  /* if nothing inserted, return Id 0 */
  if (!*space)
    return 0;
#if 0
  fprintf(stderr, "evr: %s\n", space);
#endif
  /* intern and create */
  return pool_str2id(pool, space, 1);
}


/* create e:v-r from attributes
 * atts is array of name,value pairs, NULL at end
 *   even index into atts is name
 *   odd index is value
 */
static Id
evr_atts2id(Pool *pool, struct parsedata *pd, const char **atts)
{
  const char *e, *v, *r;
  e = v = r = 0;
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, "epoch"))
	e = atts[1];
      else if (!strcmp(*atts, "version"))
	v = atts[1];
      else if (!strcmp(*atts, "release"))
	r = atts[1];
    }
  return evr2id(pool, pd, e, v, r);
}

/*------------------------------------------------------------------*/
/* rel operator handling */

struct flagtab {
  char *from;
  int to;
};

static struct flagtab flagtab[] = {
  { ">",  REL_GT },
  { "=",  REL_EQ },
  { ">=", REL_GT|REL_EQ },
  { "<",  REL_LT },
  { "!=", REL_GT|REL_LT },
  { "<=", REL_LT|REL_EQ },
  { "(any)", REL_LT|REL_EQ|REL_GT },
  { "==", REL_EQ },
  { "gt", REL_GT },
  { "eq", REL_EQ },
  { "ge", REL_GT|REL_EQ },
  { "lt", REL_LT },
  { "ne", REL_GT|REL_LT },
  { "le", REL_LT|REL_EQ },
  { "gte", REL_GT|REL_EQ },
  { "lte", REL_LT|REL_EQ },
  { "GT", REL_GT },
  { "EQ", REL_EQ },
  { "GE", REL_GT|REL_EQ },
  { "LT", REL_LT },
  { "NE", REL_GT|REL_LT },
  { "LE", REL_LT|REL_EQ }
};

/*
 * process new dependency from parser
 *  olddeps = already collected deps, this defines the 'kind' of dep
 *  atts = array of name,value attributes of dep
 *  isreq == 1 if its a requires
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, const char **atts, Id marker)
{
  Id id, name;
  const char *n, *f, *k;
  const char **a;

  n = f = k = NULL;

  /* loop over name,value pairs */
  for (a = atts; *a; a += 2)
    {
      if (!strcmp(*a, "name"))
	n = a[1];
      if (!strcmp(*a, "kind"))
	k = a[1];
      else if (!strcmp(*a, "op"))
	f = a[1];
      else if (marker && !strcmp(*a, "pre") && a[1][0] == '1')
        marker = SOLVABLE_PREREQMARKER;
    }
  if (!n)			       /* quit if no name found */
    return olddeps;

  /* kind, name */
  if (k && !strcmp(k, "package"))
    k = NULL;			       /* package is default */

  if (k)			       /* if kind!=package, intern <kind>:<name> */
    {
      int l = strlen(k) + 1 + strlen(n) + 1;
      char *space = solv_xmlparser_contentspace(&pd->xmlp, l);
      sprintf(space, "%s:%s", k, n);
      name = pool_str2id(pool, space, 1);
    }
  else
    {
      name = pool_str2id(pool, n, 1);       /* package: just intern <name> */
    }

  if (f)			       /* operator ? */
    {
      /* intern e:v-r */
      Id evr = evr_atts2id(pool, pd, atts);
      /* parser operator to flags */
      int flags;
      for (flags = 0; flags < sizeof(flagtab)/sizeof(*flagtab); flags++)
	if (!strcmp(f, flagtab[flags].from))
	  {
	    flags = flagtab[flags].to;
	    break;
	  }
      if (flags > 7)
	flags = 0;
      /* intern rel */
      id = pool_rel2id(pool, name, evr, flags, 1);
    }
  else
    id = name;			       /* no operator */

  /* add new dependency to repo */
  return repo_addid_dep(pd->repo, olddeps, id, marker);
}


/*----------------------------------------------------------------*/

static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;

  switch (state)
    {

    case STATE_NAME:
      if (pd->kind)		       /* if kind is set (non package) */
        {
          strcpy(xmlp->content, pd->kind);
          xmlp->lcontent = strlen(xmlp->content);
	  xmlp->content[xmlp->lcontent++] = ':';   /* prefix name with '<kind>:' */
	  xmlp->content[xmlp->lcontent] = 0;
	}
      break;

    case STATE_PACKAGE:		       /* solvable name */
      pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
      pd->srcpackage = 0;
      pd->kind = NULL;		       /* default is (src)package */
      if (!strcmp(name, "selection"))
        pd->kind = "selection";
      else if (!strcmp(name, "pattern"))
        pd->kind = "pattern";
      else if (!strcmp(name, "atom"))
        pd->kind = "atom";
      else if (!strcmp(name, "product"))
        pd->kind = "product";
      else if (!strcmp(name, "patch"))
        pd->kind = "patch";
      else if (!strcmp(name, "application"))
        pd->kind = "application";
      else if (!strcmp(name, "srcpackage"))
	pd->srcpackage = 1;
      pd->levrspace = 1;
      pd->epoch = 0;
      pd->version = 0;
      pd->release = 0;
      pd->freshens = 0;
#if 0
      fprintf(stderr, "package #%d\n", s - pool->solvables);
#endif
      break;

    case STATE_UPDATE:
      pd->levrspace = 1;
      pd->epoch = 0;
      pd->version = 0;
      pd->release = 0;
      break;

    case STATE_PROVIDES:	       /* start of provides */
      s->provides = 0;
      break;
    case STATE_PROVIDESENTRY:	       /* entry within provides */
      s->provides = adddep(pool, pd, s->provides, atts, 0);
      break;
    case STATE_REQUIRESENTRY:
      s->requires = adddep(pool, pd, s->requires, atts, -SOLVABLE_PREREQMARKER);
      break;
    case STATE_PREREQUIRESENTRY:
      s->requires = adddep(pool, pd, s->requires, atts, SOLVABLE_PREREQMARKER);
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
    default:
      break;
    }
}

static const char *
findKernelFlavor(struct parsedata *pd, Solvable *s)
{
  Pool *pool = pd->pool;
  Id pid, *pidp;

  if (s->provides)
    {
      pidp = pd->repo->idarraydata + s->provides;
      while ((pid = *pidp++) != 0)
	{
	  Reldep *prd;
	  const char *depname;

	  if (!ISRELDEP(pid))
	    continue;               /* wrong provides name */
	  prd = GETRELDEP(pool, pid);
	  depname = pool_id2str(pool, prd->name);
	  if (!strncmp(depname, "kernel-", 7))
	    return depname + 7;
	}
    }

  if (s->requires)
    {
      pidp = pd->repo->idarraydata + s->requires;
      while ((pid = *pidp++) != 0)
	{
	  const char *depname;

	  if (!ISRELDEP(pid))
	    {
	      depname = pool_id2str(pool, pid);
	    }
	  else
	    {
	      Reldep *prd = GETRELDEP(pool, pid);
	      depname = pool_id2str(pool, prd->name);
	    }
	  if (!strncmp(depname, "kernel-", 7))
	    return depname + 7;
	}
    }

  return 0;
}


static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  Id evr;
  unsigned int t = 0;
  const char *flavor;

  switch (state)
    {

    case STATE_PACKAGE:		       /* package complete */
      if (pd->srcpackage && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->arch = ARCH_SRC;
      if (!s->arch)                    /* default to "noarch" */
	s->arch = ARCH_NOARCH;

      if (!s->evr && pd->version)      /* set solvable evr */
        s->evr = evr2id(pool, pd,
                        pd->epoch   ? pd->evrspace + pd->epoch   : 0,
                        pd->version ? pd->evrspace + pd->version : 0,
                        pd->release ? pd->evrspace + pd->release : 0);
      /* ensure self-provides */
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      repo_rewrite_suse_deps(s, pd->freshens);
      pd->freshens = 0;

      /* see bugzilla bnc#190163 */
      flavor = findKernelFlavor(pd, s);
      if (flavor)
	{
	  char *cflavor = solv_strdup(flavor);	/* make pointer safe */

	  Id npr;
	  Id pid;

	  /* this is either a kernel package or a kmp */
	  if (s->provides)
	    {
	      Offset prov = s->provides;
	      npr = 0;
	      while ((pid = pd->repo->idarraydata[prov++]) != 0)
		{
		  const char *depname = 0;
		  Reldep *prd = 0;

		  if (ISRELDEP(pid))
		    {
		      prd = GETRELDEP(pool, pid);
		      depname = pool_id2str(pool, prd->name);
		    }
		  else
		    {
		      depname = pool_id2str(pool, pid);
		    }


		  if (!strncmp(depname, "kernel(", 7) && !strchr(depname, ':'))
		    {
		      char newdep[100];
		      snprintf(newdep, sizeof(newdep), "kernel(%s:%s", cflavor, depname + 7);
		      pid = pool_str2id(pool, newdep, 1);
		      if (prd)
			pid = pool_rel2id(pool, pid, prd->evr, prd->flags, 1);
		    }

		  npr = repo_addid_dep(pd->repo, npr, pid, 0);
		}
	      s->provides = npr;
	    }
#if 1

	  if (s->requires)
	    {
	      Offset reqs = s->requires;
	      npr = 0;
	      while ((pid = pd->repo->idarraydata[reqs++]) != 0)
		{
		  const char *depname = 0;
		  Reldep *prd = 0;

		  if (ISRELDEP(pid))
		    {
		      prd = GETRELDEP(pool, pid);
		      depname = pool_id2str(pool, prd->name);
		    }
		  else
		    {
		      depname = pool_id2str(pool, pid);
		    }

		  if (!strncmp(depname, "kernel(", 7) && !strchr(depname, ':'))
		    {
		      char newdep[100];
		      snprintf(newdep, sizeof(newdep), "kernel(%s:%s", cflavor, depname + 7);
		      pid = pool_str2id(pool, newdep, 1);
		      if (prd)
			pid = pool_rel2id(pool, pid, prd->evr, prd->flags, 1);
		    }
		  npr = repo_addid_dep(pd->repo, npr, pid, 0);
		}
	      s->requires = npr;
	    }
#endif
	  free(cflavor);
	}
      break;
    case STATE_NAME:
      s->name = pool_str2id(pool, content, 1);
      break;
    case STATE_VENDOR:
      s->vendor = pool_str2id(pool, content, 1);
      break;
    case STATE_BUILDTIME:
      t = atoi(content);
      if (t)
	repodata_set_num(pd->data, s - pool->solvables, SOLVABLE_BUILDTIME, t);
      break;	
    case STATE_UPDATE:		       /* new version, keeping all other metadata */
      evr = evr2id(pool, pd,
                   pd->epoch   ? pd->evrspace + pd->epoch   : 0,
                   pd->version ? pd->evrspace + pd->version : 0,
                   pd->release ? pd->evrspace + pd->release : 0);
      pd->levrspace = 1;
      pd->epoch = 0;
      pd->version = 0;
      pd->release = 0;
      /* use highest evr */
      if (!s->evr || pool_evrcmp(pool, s->evr, evr, EVRCMP_COMPARE) <= 0)
	s->evr = evr;
      break;
    case STATE_EPOCH:
    case STATE_VERSION:
    case STATE_RELEASE:
      /* ensure buffer space */
      if (xmlp->lcontent + 1 + pd->levrspace > pd->aevrspace)
	{
	  pd->aevrspace = xmlp->lcontent + 1 + pd->levrspace + 256;
	  pd->evrspace = (char *)realloc(pd->evrspace, pd->aevrspace);
	}
      memcpy(pd->evrspace + pd->levrspace, xmlp->content, xmlp->lcontent + 1);
      if (state == STATE_EPOCH)
	pd->epoch = pd->levrspace;
      else if (state == STATE_VERSION)
	pd->version = pd->levrspace;
      else
	pd->release = pd->levrspace;
      pd->levrspace += xmlp->lcontent + 1;
      break;
    case STATE_ARCH:
      s->arch = pool_str2id(pool, content, 1);
      break;
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pd->ret = pool_error(pd->pool, -1, "%s at line %u", errstr, line);
}


/*-------------------------------------------------------------------*/

/*
 * read 'helix' type xml from fp
 * add packages to pool/repo
 *
 */

int
repo_add_helix(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  Repodata *data;
  unsigned int now;

  now = solv_timems(0);
  data = repo_add_repodata(repo, flags);

  /* prepare parsedata */
  memset(&pd, 0, sizeof(pd));
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);

  pd.evrspace = (char *)solv_malloc(256);
  pd.aevrspace = 256;
  pd.levrspace = 1;

  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);

  solv_free(pd.evrspace);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo_add_helix took %d ms\n", solv_timems(now));
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo size: %d solvables\n", repo->nsolvables);
  POOL_DEBUG(SOLV_DEBUG_STATS, "repo memory used: %d K incore, %d K idarray\n", repodata_memused(data)/1024, repo->idarraysize / (int)(1024/sizeof(Id)));
  return pd.ret;
}
