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
#include "util.h"
#include "repo_rpmmd.h"


enum state {
  STATE_START,
  STATE_METADATA,
  STATE_PACKAGE,
  STATE_PRODUCT,
  STATE_PATTERN,
  STATE_PATCH,
  STATE_NAME,
  STATE_ARCH,
  STATE_VERSION,
  STATE_FORMAT,
  STATE_VENDOR,
  STATE_PROVIDES,
  STATE_PROVIDESENTRY,
  STATE_REQUIRES,
  STATE_REQUIRESENTRY,
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
  STATE_FILE,
  NUMSTATES
};


struct stateswitch {
  enum state from;
  char *ename;
  enum state to;
  int docontent;
};

static struct stateswitch stateswitches[] = {
  { STATE_START,       "metadata",        STATE_METADATA, 0 },
  { STATE_METADATA,    "package",         STATE_PACKAGE, 0 },
  { STATE_METADATA,    "product",         STATE_PRODUCT, 0 },
  { STATE_METADATA,    "pattern",         STATE_PATTERN, 0 },
  { STATE_METADATA,    "patch",           STATE_PATCH, 0 },
  { STATE_PACKAGE,     "name",            STATE_NAME, 1 },
  { STATE_PACKAGE,     "arch",            STATE_ARCH, 1 },
  { STATE_PACKAGE,     "version",         STATE_VERSION, 0 },
  { STATE_PACKAGE,     "format",          STATE_FORMAT, 0 },
  { STATE_FORMAT,      "rpm:vendor",      STATE_VENDOR, 1 },
  { STATE_FORMAT,      "vendor",          STATE_VENDOR, 1 },
  { STATE_FORMAT,      "rpm:provides",    STATE_PROVIDES, 0 },
  { STATE_FORMAT,      "provides",        STATE_PROVIDES, 0 },
  { STATE_FORMAT,      "rpm:requires",    STATE_REQUIRES, 0 },
  { STATE_FORMAT,      "requires",        STATE_REQUIRES, 0 },
  { STATE_FORMAT,      "rpm:obsoletes",   STATE_OBSOLETES , 0 },
  { STATE_FORMAT,      "obsoletes",       STATE_OBSOLETES , 0 },
  { STATE_FORMAT,      "rpm:conflicts",   STATE_CONFLICTS , 0 },
  { STATE_FORMAT,      "conflicts",       STATE_CONFLICTS , 0 },
  { STATE_FORMAT,      "rpm:recommends",  STATE_RECOMMENDS , 0 },
  { STATE_FORMAT,      "recommends",      STATE_RECOMMENDS , 0 },
  { STATE_FORMAT,      "rpm:supplements", STATE_SUPPLEMENTS, 0 },
  { STATE_FORMAT,      "supplements",     STATE_SUPPLEMENTS, 0 },
  { STATE_FORMAT,      "rpm:suggests",    STATE_SUGGESTS, 0 },
  { STATE_FORMAT,      "suggests",        STATE_SUGGESTS, 0 },
  { STATE_FORMAT,      "rpm:enhances",    STATE_ENHANCES, 0 },
  { STATE_FORMAT,      "enhances",        STATE_ENHANCES, 0 },
  { STATE_FORMAT,      "rpm:freshens",    STATE_FRESHENS, 0 },
  { STATE_FORMAT,      "freshens",        STATE_FRESHENS, 0 },
  { STATE_FORMAT,      "file",            STATE_FILE, 1 },
  { STATE_PROVIDES,    "rpm:entry",       STATE_PROVIDESENTRY, 0 },
  { STATE_REQUIRES,    "rpm:entry",       STATE_REQUIRESENTRY, 0 },
  { STATE_OBSOLETES,   "rpm:entry",       STATE_OBSOLETESENTRY, 0 },
  { STATE_CONFLICTS,   "rpm:entry",       STATE_CONFLICTSENTRY, 0 },
  { STATE_RECOMMENDS,  "rpm:entry",       STATE_RECOMMENDSENTRY, 0 },
  { STATE_SUPPLEMENTS, "rpm:entry",       STATE_SUPPLEMENTSENTRY, 0 },
  { STATE_SUGGESTS,    "rpm:entry",       STATE_SUGGESTSENTRY, 0 },
  { STATE_ENHANCES,    "rpm:entry",       STATE_ENHANCESENTRY, 0 },
  { STATE_FRESHENS,    "rpm:entry",       STATE_FRESHENSENTRY, 0 },
  { NUMSTATES}
};

struct parsedata {
  char *kind;
  char *tmp;
  int tmpl;
  int depth;
  enum state state;
  int statedepth;
  char *content;
  int lcontent;
  int acontent;
  int docontent;
  int numpacks;
  Pool *pool;
  Repo *repo;
  Solvable *solvable;
  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
};

static char *
join(struct parsedata *pd, char *s1, char *s2, char *s3)
{
  int l = 1;
  char *p;

  if (s1)
    l += strlen(s1);
  if (s2)
    l += strlen(s2);
  if (s3)
    l += strlen(s3);
  if (l > pd->tmpl)
    {
      pd->tmpl = l + 256;
      if (!pd->tmp)
        pd->tmp = malloc(pd->tmpl);
      else
        pd->tmp = realloc(pd->tmp, pd->tmpl);
    }
  p = pd->tmp;
  if (s1)
    {
      strcpy(p, s1);
      p += strlen(s1);
    }
  if (s2)
    {
      strcpy(p, s2);
      p += strlen(s2);
    }
  if (s3)
    {
      strcpy(p, s3);
      p += strlen(s3);
    }
  return pd->tmp;
}


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
      pd->content = sat_realloc(pd->content, l + 256);
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

static char *flagtab[] = {
  "GT",
  "EQ",
  "GE",
  "LT",
  "NE",
  "LE"
};

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
  if (k && !strcmp(k, "package"))
    k = 0;
  if (k)
    {
      int l = strlen(k) + 1 + strlen(n) + 1;
      if (l > pd->acontent)
	{
	  pd->content = sat_realloc(pd->content, l + 256);
	  pd->acontent = l + 256;
	}
      sprintf(pd->content, "%s:%s", k, n); 
      name = str2id(pool, pd->content, 1); 
    }
  else
    name = str2id(pool, (char *)n, 1);
  if (f)
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

  if (pd->depth != pd->statedepth)
    {
      pd->depth++;
      return;
    }
  pd->depth++;
  for (sw = pd->swtab[pd->state]; sw->from == pd->state; sw++)
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
    case STATE_METADATA:
      for (; *atts; atts += 2)
	{
	  if (!strcmp(*atts, "packages"))
	    {
	      pd->numpacks = atoi(atts[1]);
	      if (pd->numpacks < 0)
		pd->numpacks = 0;
#if 0
	      fprintf(stderr, "numpacks: %d\n", pd->numpacks);
#endif
	      pd->solvable = pool_id2solvable(pool, repo_add_solvable_block(pd->repo, pd->numpacks));
	    }
	}
      break;
    case STATE_PACKAGE:
      if (pd->numpacks == 0)
	pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
#if 0
      fprintf(stderr, "package #%d\n", pd->solvable - pool->solvables);
#endif
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
      s->freshens = 0;
      break;
    case STATE_FRESHENSENTRY:
      s->freshens = adddep(pool, pd, s->freshens, atts, 0);
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
  Id id;

  if (pd->depth != pd->statedepth)
    {
      pd->depth--;
      // printf("back from unknown %d %d %d\n", pd->state, pd->depth, pd->statedepth);
      return;
    }
  pd->depth--;
  pd->statedepth--;
  switch (pd->state)
    {
    case STATE_PATTERN:
    case STATE_PRODUCT:
      if ( pd->state == STATE_PATTERN )
        pd->kind = "pattern";
      if ( pd->state == STATE_PRODUCT )
        pd->kind = "product";
    case STATE_PACKAGE:
      if (!s->arch)
        s->arch = ARCH_NOARCH;
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        s->provides = repo_addid_dep(pd->repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      s->supplements = repo_fix_legacy(pd->repo, s->provides, s->supplements);
      if (pd->numpacks > 0)
	{
	  pd->numpacks--;
	  pd->solvable++;
	}
      break;
    case STATE_NAME:
      if ( pd->kind )
        {
          s->name = str2id(pool, pd->content, 1);
          s->name = str2id(pool, join(pd, pd->kind, ":", pd->content), 1);
        }
      else
        {
          s->name = str2id(pool, pd->content, 1);
        }
        pd->kind = 0;
      break;
    case STATE_ARCH:
      s->arch = str2id(pool, pd->content, 1);
      break;
    case STATE_VENDOR:
      s->vendor = str2id(pool, pd->content, 1);
      break;
    case STATE_FILE:
      id = str2id(pool, pd->content, 1);
      s->provides = repo_addid(pd->repo, s->provides, id);
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
      pd->content = sat_realloc(pd->content, l + 256);
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
repo_add_rpmmd(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  char buf[BUFF_SIZE];
  int i, l;
  struct stateswitch *sw;

  memset(&pd, 0, sizeof(pd));
  for (i = 0, sw = stateswitches; sw->from != NUMSTATES; i++, sw++)
    {
      if (!pd.swtab[sw->from])
        pd.swtab[sw->from] = sw;
      pd.sbtab[sw->to] = sw->from;
    }
  pd.pool = pool;
  pd.repo = repo;
  pd.content = sat_malloc(256);
  pd.acontent = 256;
  pd.lcontent = 0;
  pd.tmp = 0;
  pd.tmpl = 0;
  pd.kind = 0;
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &pd);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  fprintf(stderr, "%s at line %u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser));
	  exit(1);
	}
      if (l == 0)
	break;
    }
  XML_ParserFree(parser);
  if (pd.numpacks)
    repo_free_solvable_block(repo, pd.solvable - pool->solvables, pd.numpacks, 1);
  sat_free(pd.content);
}
