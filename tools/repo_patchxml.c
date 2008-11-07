/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

#include "pool.h"
#include "repo.h"
#include "repo_patchxml.h"
#include "repo_rpmmd.h"

//#define TESTMM

enum state {
  STATE_START,
  STATE_PATCH,
  STATE_ATOM,
  STATE_NAME,
  STATE_ARCH,
  STATE_VERSION,
  STATE_SUMMARY,
  STATE_DESCRIPTION,
  STATE_CATEGORY,
  STATE_PKGFILES,
  STATE_DELTARPM,
  STATE_DLOCATION,
  STATE_DCHECKSUM,
  STATE_DTIME,
  STATE_DSIZE,
  STATE_DBASEVERSION,
  STATE_REQUIRES,
  STATE_REQUIRESENTRY,
  STATE_PROVIDES,
  STATE_PROVIDESENTRY,
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
  STATE_REBOOT,
  STATE_RESTART,
  NUMSTATES
};


struct stateswitch {
  enum state from;
  char *ename;
  enum state to;
  int docontent;
};

static struct stateswitch stateswitches[] = {
  { STATE_START,       "patch",           STATE_PATCH, 0 },
  { STATE_START,       "package",         STATE_ATOM, 0 },
  { STATE_START,       "patches",         STATE_START, 0},
  { STATE_PATCH,       "yum:name",        STATE_NAME, 1 },
  { STATE_PATCH,       "yum:arch",        STATE_ARCH, 1 },
  { STATE_PATCH,       "yum:version",     STATE_VERSION, 0 },
  { STATE_PATCH,       "name",            STATE_NAME, 1 },
  { STATE_PATCH,       "arch",            STATE_ARCH, 1 },
  { STATE_PATCH,       "version",         STATE_VERSION, 0 },
  { STATE_PATCH,       "summary",         STATE_SUMMARY, 1 },
  { STATE_PATCH,       "description",     STATE_DESCRIPTION, 1 },
  { STATE_PATCH,       "category",        STATE_CATEGORY, 1 },
  { STATE_PATCH,       "reboot-needed",   STATE_REBOOT, 0 },
  { STATE_PATCH,       "package-manager", STATE_RESTART, 0 },
  { STATE_PATCH,       "rpm:requires",    STATE_REQUIRES, 0 },
  { STATE_PATCH,       "rpm:provides",    STATE_PROVIDES, 0 },
  { STATE_PATCH,       "rpm:requires",    STATE_REQUIRES, 0 },
  { STATE_PATCH,       "rpm:obsoletes",   STATE_OBSOLETES , 0 },
  { STATE_PATCH,       "rpm:conflicts",   STATE_CONFLICTS , 0 },
  { STATE_PATCH,       "rpm:recommends" , STATE_RECOMMENDS , 0 },
  { STATE_PATCH,       "rpm:supplements", STATE_SUPPLEMENTS, 0 },
  { STATE_PATCH,       "rpm:suggests",    STATE_SUGGESTS, 0 },
  { STATE_PATCH,       "rpm:enhances",    STATE_ENHANCES, 0 },
  { STATE_PATCH,       "rpm:freshens",    STATE_FRESHENS, 0 },
  { STATE_PATCH,       "suse:freshens",   STATE_FRESHENS, 0 },
  { STATE_PATCH,       "atoms", 	  STATE_START, 0 },
  { STATE_PATCH,       "pkgfiles",        STATE_PKGFILES, 0 },
  { STATE_PKGFILES,    "deltarpm",        STATE_DELTARPM, 0 },
  { STATE_PKGFILES,    "patchrpm",        STATE_DELTARPM, 0 },
  { STATE_DELTARPM,    "location",        STATE_DLOCATION, 0 },
  { STATE_DELTARPM,    "checksum",        STATE_DCHECKSUM, 1 },
  { STATE_DELTARPM,    "time",            STATE_DTIME, 0 },
  { STATE_DELTARPM,    "size",            STATE_DSIZE, 0 },
  { STATE_DELTARPM,    "base-version",    STATE_DBASEVERSION, 0 },
  { STATE_PROVIDES,    "rpm:entry",       STATE_PROVIDESENTRY, 0 },
  { STATE_REQUIRES,    "rpm:entry",       STATE_REQUIRESENTRY, 0 },
  { STATE_OBSOLETES,   "rpm:entry",       STATE_OBSOLETESENTRY, 0 },
  { STATE_CONFLICTS,   "rpm:entry",       STATE_CONFLICTSENTRY, 0 },
  { STATE_RECOMMENDS,  "rpm:entry",       STATE_RECOMMENDSENTRY, 0 },
  { STATE_SUPPLEMENTS, "rpm:entry",       STATE_SUPPLEMENTSENTRY, 0 },
  { STATE_SUGGESTS,    "rpm:entry",       STATE_SUGGESTSENTRY, 0 },
  { STATE_ENHANCES,    "rpm:entry",       STATE_ENHANCESENTRY, 0 },
  { STATE_FRESHENS,    "rpm:entry",       STATE_FRESHENSENTRY, 0 },
  { STATE_FRESHENS,    "suse:entry",      STATE_FRESHENSENTRY, 0 },
  { NUMSTATES}
};

/* Cumulated info about the current deltarpm or patchrpm */
struct deltarpm {
  Id locdir;
  Id locname;
  Id locevr;
  Id locsuffix;
  unsigned buildtime;
  unsigned downloadsize, archivesize;
  char *filechecksum;
  /* Baseversions.  deltarpm only has one, patchrpm may have more.  */
  Id *bevr;
  unsigned nbevr;
  /* If deltarpm, then this is filled.  */
  Id seqname;
  Id seqevr;
  char *seqnum;
};

struct parsedata {
  int depth;
  enum state state;
  int statedepth;
  char *content;
  int lcontent;
  int acontent;
  int docontent;
  Pool *pool;
  Repo *repo;
  Repodata *data;
  unsigned int datanum;
  Solvable *solvable;
  char *kind;
  Offset freshens;
  unsigned int timestamp;
  
  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
  char *tempstr;
  int ltemp;
  int atemp;
  struct deltarpm delta;
};

#if 0
static void
append_str(struct parsedata *pd, const char *s)
{
  if (!s)
    return;
  int l = pd->ltemp + strlen(s) + 1;
  if (l > pd->atemp)
    {
      pd->tempstr = realloc(pd->tempstr, l + 256);
      pd->atemp = l + 256;
    }
  strcpy(pd->tempstr + pd->ltemp, s);
  pd->ltemp += strlen(s);
}
#endif

/*
 * create evr (as Id) from 'epoch', 'ver' and 'rel' attributes
 */

static Id
makeevr_atts(Pool *pool, struct parsedata *pd, const char **atts)
{
  const char *e, *v, *r, *v2;
  char *c;
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
  if (e && !strcmp(e, "0"))
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
  if (l > pd->acontent)
    {
      pd->content = realloc(pd->content, l + 256);
      pd->acontent = l + 256;
    }
  c = pd->content;
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
  if (!*pd->content)
    return 0;
#if 0
  fprintf(stderr, "evr: %s\n", pd->content);
#endif
  return str2id(pool, pd->content, 1);
}


/*
 * find attribute
 */

static const char *
find_attr(const char *txt, const char **atts)
{
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, txt))
        return atts[1];
    }
  return 0;
}


/*
 * relation comparision operators
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
 * add dependency
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, const char **atts, int isreq)
{
  Id id, name, marker;
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
  if (k && !strcmp(k, "package"))            /* kind 'package' -> ignore */
    k = 0;
  if (k)
    {
      int l = strlen(k) + 1 + strlen(n) + 1;
      if (l > pd->acontent)
	{
	  pd->content = realloc(pd->content, l + 256);
	  pd->acontent = l + 256;
	}
      sprintf(pd->content, "%s:%s", k, n);   /* prepend kind to name */
      name = str2id(pool, pd->content, 1); 
    }
  else
    name = str2id(pool, (char *)n, 1);
  if (f)                                     /* flags means name,operator,relation */
    {
      Id evr = makeevr_atts(pool, pd, atts);
      int flags;
      for (flags = 0; flags < 6; flags++)
	if (!strcmp(f, flagtab[flags]))
	  break;
      flags = flags < 6 ? flags + 1 : 0;
      id = rel2id(pool, name, evr, flags, 1);
    }
  else
    id = name;
#if 0
  fprintf(stderr, "new dep %s%s%s\n", id2str(pool, d), id2rel(pool, d), id2evr(pool, d));
#endif
  return repo_addid_dep(pd->repo, olddeps, id, marker);
}


static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  struct stateswitch *sw;
  const char *str;

  if (pd->depth != pd->statedepth)
    {
      pd->depth++;
      return;
    }

  if (pd->state == STATE_PATCH && !strcmp(name, "format"))
    return;

  pd->depth++;
  if (!pd->swtab[pd->state])
    return;
  for (sw = pd->swtab[pd->state]; sw->from == pd->state; sw++)  /* find name in statetable */
    if (!strcmp(sw->ename, name))
      break;
  
  if (sw->from != pd->state)
    {
#if 0
      fprintf(stderr, "into unknown: %s\n", name);
#endif
      return;
    }
  pd->state = sw->to;
  pd->docontent = sw->docontent;
  pd->statedepth = pd->depth;
  pd->lcontent = 0;
  *pd->content = 0;
  switch(pd->state)
    {
    case STATE_NAME:
      if (pd->kind)
        {
          strcpy(pd->content, pd->kind);
          pd->lcontent = strlen(pd->content);
          pd->content[pd->lcontent++] = ':';
          pd->content[pd->lcontent] = 0;
        }
      break;
    case STATE_PATCH:
      if (sw->from == STATE_START)
        {
	  if ((str = find_attr("timestamp", atts)))
	    {
	      pd->timestamp = strtoul(str, NULL, 10);
	    }
        }
      /*FALLTHRU*/
    case STATE_ATOM:
      if (pd->state == STATE_ATOM)
	{
	  /* HACK: close patch */
	  if (pd->kind && !strcmp(pd->kind, "patch"))
	    {
	      if (!s->arch)
		s->arch = ARCH_NOARCH;
	      s->provides = repo_addid_dep(pd->repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	    }
	  pd->kind = "atom";
	  pd->state = STATE_PATCH;
	}
      else
        pd->kind = "patch";
      
      pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
      pd->freshens = 0;

      if (!strcmp(pd->kind, "patch"))
        {
          pd->datanum = pd->solvable - pool->solvables;
          repodata_set_num(pd->data, pd->datanum, SOLVABLE_BUILDTIME, pd->timestamp);
	}
#if 0
      fprintf(stderr, "package #%d\n", pd->solvable - pool->solvables);
#endif
      break;
    case STATE_DELTARPM:
      memset(&pd->delta, 0, sizeof (pd->delta));
      *pd->tempstr = 0;
      pd->ltemp = 0;
      break;
    case STATE_DLOCATION:
      if ((str = find_attr("href", atts)))
        {
	  /* Separate the filename into its different parts.
	     rpm/x86_64/alsa-1.0.14-31_31.2.x86_64.delta.rpm
	     --> dir = rpm/x86_64
	         name = alsa
		 evr = 1.0.14-31_31.2
		 suffix = x86_64.delta.rpm.  */
          char *real_str = strdup(str);
	  char *s = real_str;
          char *s1, *s2;
	  s1 = strrchr (s, '/');
	  if (s1)
	    {
	      pd->delta.locdir = strn2id(pool, s, s1 - s, 1);
	      s = s1 + 1;
	    }
	  /* Guess suffix.  */
	  s1 = strrchr (s, '.');
	  if (s1)
	    {
	      for (s2 = s1 - 1; s2 > s; s2--)
	        if (*s2 == '.')
		  break;
	      if (!strcmp (s2, ".delta.rpm") || !strcmp (s2, ".patch.rpm"))
		{
	          s1 = s2;
		  /* We accept one more item as suffix.  */
		  for (s2 = s1 - 1; s2 > s; s2--)
		    if (*s2 == '.')
		      break;
		  s1 = s2;
		}
	      if (*s1 == '.')
	        *s1++ = 0;
	      pd->delta.locsuffix = str2id(pool, s1, 1); 
	    }
	  /* Last '-'.  */
	  s1 = strrchr (s, '-');
	  if (s1)
	    {
	      /* Second to last '-'.  */
	      for (s2 = s1 - 1; s2 > s; s2--)
	        if (*s2 == '-')
	          break;
	    }
	  else
	    s2 = 0;
	  if (s2 > s && *s2 == '-')
	    {
	      *s2++ = 0;
	      pd->delta.locevr = str2id(pool, s2, 1);
	    }
	  pd->delta.locname = str2id(pool, s, 1);
	  free(real_str);
        }
      break;
    case STATE_DTIME:
      str = find_attr("build", atts);
      if (str)
	pd->delta.buildtime = atoi(str);
      break;
    case STATE_DSIZE:
      if ((str = find_attr("package", atts)))
	pd->delta.downloadsize = atoi(str);
      if ((str = find_attr("archive", atts)))
	pd->delta.archivesize = atoi(str);
      break;
    case STATE_DBASEVERSION:
      if ((str = find_attr("sequence_info", atts)))
	{
	  const char *s1, *s2;
	  s1 = strrchr(str, '-');
	  if (s1)
	    {
	      for (s2 = s1 - 1; s2 > str; s2--)
	        if (*s2 == '-')
		  break;
	      if (*s2 == '-')
	        {
		  for (s2 = s2 - 1; s2 > str; s2--)
		    if (*s2 == '-')
		      break;
		  if (*s2 == '-')
		    {
		      pd->delta.seqevr = strn2id(pool, s2 + 1, s1 - s2 - 1, 1);
		      pd->delta.seqname = strn2id(pool, str, s2 - str, 1);
		      str = s1 + 1;
		    }
		}
	    }
	  pd->delta.seqnum = strdup(str);
	}
      pd->delta.nbevr++;
      pd->delta.bevr = sat_realloc (pd->delta.bevr, pd->delta.nbevr * sizeof(Id));
      pd->delta.bevr[pd->delta.nbevr - 1] = makeevr_atts(pool, pd, atts);
      break;
    case STATE_VERSION:
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
    case STATE_REBOOT:
      repodata_set_void(pd->data, pd->datanum, UPDATE_REBOOT);
      break;  
    case STATE_RESTART:
      repodata_set_void(pd->data, pd->datanum, UPDATE_RESTART);
      break;  
    default:
      break;
    }
}

static void XMLCALL
endElement(void *userData, const char *name)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;

  if (pd->depth != pd->statedepth)
    {
      pd->depth--;
      // printf("back from unknown %d %d %d\n", pd->state, pd->depth, pd->statedepth);
      return;
    }

  if (pd->state == STATE_PATCH && !strcmp(name, "format"))
    return;

  pd->depth--;
  pd->statedepth--;
  switch (pd->state)
    {
    case STATE_PATCH:
      if (!strcmp(name, "patch") && strcmp(pd->kind, "patch"))
	break;	/* already closed */
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        s->provides = repo_addid_dep(pd->repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      s->supplements = repo_fix_supplements(pd->repo, s->provides, s->supplements, pd->freshens);
      s->conflicts = repo_fix_conflicts(pd->repo, s->conflicts);
      pd->freshens = 0;
      break;
    case STATE_NAME:
      s->name = str2id(pool, pd->content, 1);
      break;
    case STATE_ARCH:
      s->arch = str2id(pool, pd->content, 1);
      break;
    case STATE_SUMMARY:
      repodata_set_str(pd->data, pd->datanum, SOLVABLE_SUMMARY, pd->content);
      break;
    case STATE_DESCRIPTION:
      repodata_set_str(pd->data, pd->datanum, SOLVABLE_DESCRIPTION, pd->content);
      break;
    case STATE_CATEGORY:  
      repodata_set_str(pd->data, pd->datanum, SOLVABLE_PATCHCATEGORY, pd->content);
      break;
    case STATE_DELTARPM:
#ifdef TESTMM
      {
	int i;
        struct deltarpm *d = &pd->delta;
	fprintf (stderr, "found deltarpm for %s:\n", id2str(pool, s->name));
	fprintf (stderr, "   loc: %s %s %s %s\n", id2str(pool, d->locdir),
	         id2str(pool, d->locname), id2str(pool, d->locevr),
		 id2str(pool, d->locsuffix));
	fprintf (stderr, "  time: %u\n", d->buildtime);
	fprintf (stderr, "  size: %d down, %d archive\n", d->downloadsize,
		 d->archivesize);
	fprintf (stderr, "  chek: %s\n", d->filechecksum);
	if (d->seqnum)
	  {
	    fprintf (stderr, "  base: %s, seq: %s %s %s\n",
		     id2str(pool, d->bevr[0]), id2str(pool, d->seqname),
		     id2str(pool, d->seqevr), d->seqnum);
	    if (d->seqevr != d->bevr[0])
	      fprintf (stderr, "XXXXX evr\n");
	    /* Name of package ("atom:xxxx") should match the sequence info
	       name.  */
	    if (strcmp(id2str(pool, d->seqname), id2str(pool, s->name) + 5))
	      fprintf (stderr, "XXXXX name\n");
	  }
	else
	  {
	    fprintf (stderr, "  base:");
	    for (i = 0; i < d->nbevr; i++)
	      fprintf (stderr, " %s", id2str(pool, d->bevr[i]));
	    fprintf (stderr, "\n");
	  }
      }
#endif
      free(pd->delta.filechecksum);
      free(pd->delta.bevr);
      free(pd->delta.seqnum);
      break;
    case STATE_DCHECKSUM:
      pd->delta.filechecksum = strdup(pd->content);
      break;
    default:
      break;
    }
  pd->state = pd->sbtab[pd->state];
  pd->docontent = 0;
  // printf("back from known %d %d %d\n", pd->state, pd->depth, pd->statedepth);
}

static void XMLCALL
characterData(void *userData, const XML_Char *s, int len)
{
  struct parsedata *pd = userData;
  int l;
  char *c;

  if (!pd->docontent)
    return;
  l = pd->lcontent + len + 1;
  if (l > pd->acontent)
    {
      pd->content = realloc(pd->content, l + 256);
      pd->acontent = l + 256;
    }
  c = pd->content + pd->lcontent;
  pd->lcontent += len;
  while (len-- > 0)
    *c++ = *s++;
  *c = 0;
}


#define BUFF_SIZE 8192

void
repo_add_patchxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  char buf[BUFF_SIZE];
  int i, l;
  struct stateswitch *sw;
  Repodata *data;

  if (!(flags & REPO_REUSE_REPODATA))
    data = repo_add_repodata(repo, 0);
  else
    data = repo_last_repodata(repo);

  memset(&pd, 0, sizeof(pd));
  for (i = 0, sw = stateswitches; sw->from != NUMSTATES; i++, sw++)
    {
      if (!pd.swtab[sw->from])
        pd.swtab[sw->from] = sw;
      pd.sbtab[sw->to] = sw->from;
    }
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;

  pd.content = malloc(256);
  pd.acontent = 256;
  pd.lcontent = 0;
  pd.tempstr = malloc(256);
  pd.atemp = 256;
  pd.ltemp = 0;
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &pd);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  pool_debug(pool, SAT_FATAL, "repo_patchxml: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
	  exit(1);
	}
      if (l == 0)
	break;
    }
  XML_ParserFree(parser);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);

  free(pd.content);
}
