#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

#include "pool.h"
#include "source_rpmmd.h"


enum state {
  STATE_START,
  STATE_METADATA,
  STATE_PACKAGE,
  STATE_NAME,
  STATE_ARCH,
  STATE_VERSION,
  STATE_FORMAT,
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
  { STATE_PACKAGE,     "name",            STATE_NAME, 1 },
  { STATE_PACKAGE,     "arch",            STATE_ARCH, 1 },
  { STATE_PACKAGE,     "version",         STATE_VERSION, 0 },
  { STATE_PACKAGE,     "format",          STATE_FORMAT, 0 },
  { STATE_FORMAT,      "rpm:provides",    STATE_PROVIDES, 0 },
  { STATE_FORMAT,      "rpm:requires",    STATE_REQUIRES, 0 },
  { STATE_FORMAT,      "rpm:obsoletes",   STATE_OBSOLETES , 0 },
  { STATE_FORMAT,      "rpm:conflicts",   STATE_CONFLICTS , 0 },
  { STATE_FORMAT,      "rpm:recommends" , STATE_RECOMMENDS , 0 },
  { STATE_FORMAT,      "rpm:supplements", STATE_SUPPLEMENTS, 0 },
  { STATE_FORMAT,      "rpm:suggests",    STATE_SUGGESTS, 0 },
  { STATE_FORMAT,      "rpm:enhances",    STATE_ENHANCES, 0 },
  { STATE_FORMAT,      "rpm:freshens",    STATE_FRESHENS, 0 },
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

struct deps {
  unsigned int provides;
  unsigned int requires;
  unsigned int obsoletes;
  unsigned int conflicts;
  unsigned int recommends;
  unsigned int supplements;
  unsigned int enhances;
  unsigned int suggests;
  unsigned int freshens;
};

struct parsedata {
  int depth;
  enum state state;
  int statedepth;
  char *content;
  int lcontent;
  int acontent;
  int docontent;
  int numpacks;
  int pack;
  Pool *pool;
  Source *source;
  Solvable *start;
  struct deps *deps;
  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
};

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
  Id id, name;
  const char *n, *f, *k;
  const char **a;

  n = f = k = 0;
  for (a = atts; *a; a += 2)
    {
      if (!strcmp(*a, "name"))
	n = a[1];
      else if (!strcmp(*a, "flags"))
	f = a[1];
      else if (!strcmp(*a, "kind"))
	k = a[1];
      else if (isreq && !strcmp(*a, "pre") && a[1][0] == '1')
	isreq = 2;
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
	  pd->content = realloc(pd->content, l + 256);
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
  return source_addid_dep(pd->source, olddeps, id, isreq);
}


static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  Solvable *s = pd->start ? pd->start + pd->pack : 0;
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
#if 0
	      fprintf(stderr, "numpacks: %d\n", pd->numpacks);
#endif
	      pool->solvables = realloc(pool->solvables, (pool->nsolvables + pd->numpacks) * sizeof(Solvable));
	      pd->start = pool->solvables + pd->source->start;
	      memset(pd->start, 0, pd->numpacks * sizeof(Solvable));
	      pd->deps = calloc(pd->numpacks, sizeof(struct deps));
	    }
	}
      break;
    case STATE_PACKAGE:
      if (pd->pack >= pd->numpacks)
	{
	  fprintf(stderr, "repomd lied about the package number\n");
	  exit(1);
	}
#if 0
      fprintf(stderr, "package #%d\n", pd->pack);
#endif
      break;
    case STATE_VERSION:
      s->evr = makeevr_atts(pool, pd, atts);
      break;
    case STATE_PROVIDES:
      pd->deps[pd->pack].provides = 0;
      break;
    case STATE_PROVIDESENTRY:
      pd->deps[pd->pack].provides = adddep(pool, pd, pd->deps[pd->pack].provides, atts, 0);
      break;
    case STATE_REQUIRES:
      pd->deps[pd->pack].requires = 0;
      break;
    case STATE_REQUIRESENTRY:
      pd->deps[pd->pack].requires = adddep(pool, pd, pd->deps[pd->pack].requires, atts, 1);
      break;
    case STATE_OBSOLETES:
      pd->deps[pd->pack].obsoletes = 0;
      break;
    case STATE_OBSOLETESENTRY:
      pd->deps[pd->pack].obsoletes = adddep(pool, pd, pd->deps[pd->pack].obsoletes, atts, 0);
      break;
    case STATE_CONFLICTS:
      pd->deps[pd->pack].conflicts = 0;
      break;
    case STATE_CONFLICTSENTRY:
      pd->deps[pd->pack].conflicts = adddep(pool, pd, pd->deps[pd->pack].conflicts, atts, 0);
      break;
    case STATE_RECOMMENDS:
      pd->deps[pd->pack].recommends = 0;
      break;
    case STATE_RECOMMENDSENTRY:
      pd->deps[pd->pack].recommends = adddep(pool, pd, pd->deps[pd->pack].recommends, atts, 0);
      break;
    case STATE_SUPPLEMENTS:
      pd->deps[pd->pack].supplements= 0;
      break;
    case STATE_SUPPLEMENTSENTRY:
      pd->deps[pd->pack].supplements = adddep(pool, pd, pd->deps[pd->pack].supplements, atts, 0);
      break;
    case STATE_SUGGESTS:
      pd->deps[pd->pack].suggests = 0;
      break;
    case STATE_SUGGESTSENTRY:
      pd->deps[pd->pack].suggests = adddep(pool, pd, pd->deps[pd->pack].suggests, atts, 0);
      break;
    case STATE_ENHANCES:
      pd->deps[pd->pack].enhances = 0;
      break;
    case STATE_ENHANCESENTRY:
      pd->deps[pd->pack].enhances = adddep(pool, pd, pd->deps[pd->pack].enhances, atts, 0);
      break;
    case STATE_FRESHENS:
      pd->deps[pd->pack].freshens = 0;
      break;
    case STATE_FRESHENSENTRY:
      pd->deps[pd->pack].freshens = adddep(pool, pd, pd->deps[pd->pack].freshens, atts, 0);
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
  Solvable *s = pd->start ? pd->start + pd->pack : 0;
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
    case STATE_PACKAGE:
#if 0
	{
	  const char *arch = id2str(pool, s->arch);
	  if (strcmp(arch, "noarch") && strcmp(arch, "i586") && strcmp(arch, "i686"))
	    break;
	}
#endif
      if (!s->arch)
        s->arch = ARCH_NOARCH;
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        pd->deps[pd->pack].provides = source_addid_dep(pd->source, pd->deps[pd->pack].provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      pd->deps[pd->pack].supplements = source_fix_legacy(pd->source, pd->deps[pd->pack].provides, pd->deps[pd->pack].supplements);
      pd->pack++;
      break;
    case STATE_NAME:
      s->name = str2id(pool, pd->content, 1);
      break;
    case STATE_ARCH:
      s->arch = str2id(pool, pd->content, 1);
      break;
    case STATE_FILE:
      id = str2id(pool, pd->content, 1);
      pd->deps[pd->pack].provides = source_addid(pd->source, pd->deps[pd->pack].provides, id);
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

Source *
pool_addsource_rpmmd(Pool *pool, FILE *fp)
{
  struct parsedata pd;
  char buf[BUFF_SIZE];
  int i, l;
  Source *source;
  Solvable *s;
  struct deps *deps;
  struct stateswitch *sw;

  source = pool_addsource_empty(pool);
  memset(&pd, 0, sizeof(pd));
  for (i = 0, sw = stateswitches; sw->from != NUMSTATES; i++, sw++)
    {
      if (!pd.swtab[sw->from])
        pd.swtab[sw->from] = sw;
      pd.sbtab[sw->to] = sw->from;
    }
  pd.pool = pool;
  pd.source = source;
  pd.content = malloc(256);
  pd.acontent = 256;
  pd.lcontent = 0;
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

  pool->nsolvables += pd.pack;
  source->nsolvables = pd.pack;

  deps = pd.deps;
  s = pool->solvables + source->start;
  for (i = 0; i < pd.pack; i++, s++)
    {
      if (deps[i].provides)
        s->provides = source->idarraydata + deps[i].provides;
      if (deps[i].requires)
        s->requires = source->idarraydata + deps[i].requires;
      if (deps[i].conflicts)
        s->conflicts = source->idarraydata + deps[i].conflicts;
      if (deps[i].obsoletes)
        s->obsoletes = source->idarraydata + deps[i].obsoletes;
      if (deps[i].recommends)
        s->recommends = source->idarraydata + deps[i].recommends;
      if (deps[i].supplements)
        s->supplements = source->idarraydata + deps[i].supplements;
      if (deps[i].suggests)
        s->suggests = source->idarraydata + deps[i].suggests;
      if (deps[i].enhances)
        s->enhances = source->idarraydata + deps[i].enhances;
      if (deps[i].freshens)
        s->freshens = source->idarraydata + deps[i].freshens;
    }
  free(deps);
  free(pd.content);
  return source;
}
