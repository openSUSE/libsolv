/*
 * source_helix.c
 * 
 * Parse 'helix' XML representation
 * and create 'source'
 * 
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

#include "source_helix.h"
#include "evr.h"


/* XML parser states */

enum state {
  STATE_START,
  STATE_CHANNEL,
  STATE_SUBCHANNEL,
  STATE_PACKAGE,
  STATE_NAME,
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

  STATE_PEPOCH,
  STATE_PVERSION,
  STATE_PRELEASE,
  STATE_PARCH,

  NUMSTATES
};

#define PACK_BLOCK 255

struct stateswitch {
  enum state from;
  char *ename;
  enum state to;
  int docontent;
};

static struct stateswitch stateswitches[] = {
  { STATE_START,       "channel",         STATE_CHANNEL, 0 },
  { STATE_CHANNEL,     "subchannel",      STATE_SUBCHANNEL, 0 },
  { STATE_SUBCHANNEL,  "package",         STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "selection",       STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "pattern",         STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "atom",            STATE_PACKAGE, 0 },
  { STATE_SUBCHANNEL,  "patch",           STATE_PACKAGE, 0 },
  { STATE_PACKAGE,     "name",            STATE_NAME, 1 },
  { STATE_PACKAGE,     "epoch",           STATE_PEPOCH, 1 },
  { STATE_PACKAGE,     "version",         STATE_PVERSION, 1 },
  { STATE_PACKAGE,     "release",         STATE_PRELEASE, 1 },
  { STATE_PACKAGE,     "arch",            STATE_PARCH, 1 },
  { STATE_PACKAGE,     "history",         STATE_HISTORY, 0 },
  { STATE_PACKAGE,     "provides",        STATE_PROVIDES, 0 },
  { STATE_PACKAGE,     "requires",        STATE_REQUIRES, 0 },
  { STATE_PACKAGE,     "obsoletes",       STATE_OBSOLETES , 0 },
  { STATE_PACKAGE,     "conflicts",       STATE_CONFLICTS , 0 },
  { STATE_PACKAGE,     "recommends" ,     STATE_RECOMMENDS , 0 },
  { STATE_PACKAGE,     "supplements",     STATE_SUPPLEMENTS, 0 },
  { STATE_PACKAGE,     "suggests",        STATE_SUGGESTS, 0 },
  { STATE_PACKAGE,     "enhances",        STATE_ENHANCES, 0 },
  { STATE_PACKAGE,     "freshens",        STATE_FRESHENS, 0 },

  { STATE_HISTORY,     "update",          STATE_UPDATE, 0 },
  { STATE_UPDATE,      "epoch",           STATE_EPOCH, 1 },
  { STATE_UPDATE,      "version",         STATE_VERSION, 1 },
  { STATE_UPDATE,      "release",         STATE_RELEASE, 1 },
  { STATE_UPDATE,      "arch",            STATE_ARCH, 1 },

  { STATE_PROVIDES,    "dep",             STATE_PROVIDESENTRY, 0 },
  { STATE_REQUIRES,    "dep",             STATE_REQUIRESENTRY, 0 },
  { STATE_OBSOLETES,   "dep",             STATE_OBSOLETESENTRY, 0 },
  { STATE_CONFLICTS,   "dep",             STATE_CONFLICTSENTRY, 0 },
  { STATE_RECOMMENDS,  "dep",             STATE_RECOMMENDSENTRY, 0 },
  { STATE_SUPPLEMENTS, "dep",             STATE_SUPPLEMENTSENTRY, 0 },
  { STATE_SUGGESTS,    "dep",             STATE_SUGGESTSENTRY, 0 },
  { STATE_ENHANCES,    "dep",             STATE_ENHANCESENTRY, 0 },
  { STATE_FRESHENS,    "dep",             STATE_FRESHENSENTRY, 0 },
  { NUMSTATES }

};

// Deps are stored as offsets into source->idarraydata
typedef struct _deps {
  Offset provides;
  Offset requires;
  Offset obsoletes;
  Offset conflicts;
  Offset recommends;
  Offset supplements;
  Offset enhances;
  Offset suggests;
  Offset freshens;
} Deps;

/*
 * parser data
 */

typedef struct _parsedata {
  // XML parser data
  int depth;
  enum state state;	// current state
  int statedepth;
  char *content;	// buffer for content of node
  int lcontent;		// actual length of current content
  int acontent;		// actual buffer size
  int docontent;	// handle content

  // source data
  int pack;             // number of solvables

  Pool *pool;		// current pool
  Source *source;	// current source
  Solvable *start;      // collected solvables

  // all dependencies
  Deps *deps;           // dependencies array, indexed by pack#

  // package data
  int  epoch;		// epoch (as offset into evrspace)
  int  version;		// version (as offset into evrspace)
  int  release;		// release (as offset into evrspace)
  char *evrspace;	// buffer for evr
  int  aevrspace;	// actual buffer space
  int  levrspace;	// actual evr length
  char *kind;

  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
} Parsedata;


/*------------------------------------------------------------------*/
/* E:V-R handling */

// create Id from epoch:version-release

static Id
evr2id(Pool *pool, Parsedata *pd, const char *e, const char *v, const char *r)
{
  char *c;
  int l;

  // treat explitcit 0 as NULL
  if (e && !strcmp(e, "0"))
    e = NULL;

  if (v && !e)
    {
      const char *v2;
      // scan version for ":"
      for (v2 = v; *v2 >= '0' && *v2 <= '9'; v2++)	// skip leading digits
        ;
      // if version contains ":", set epoch to "0"
      if (v2 > v && *v2 == ':')
	e = "0";
    }
  
  // compute length of Id string
  l = 1;  // for the \0
  if (e)
    l += strlen(e) + 1;  // e:
  if (v)
    l += strlen(v);      // v
  if (r)
    l += strlen(r) + 1;  // -r

  // extend content if not sufficient
  if (l > pd->acontent)
    {
      pd->content = (char *)realloc(pd->content, l + 256);
      pd->acontent = l + 256;
    }

  // copy e-v-r to content
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
  // if nothing inserted, return Id 0
  if (!*pd->content)
    return ID_NULL;
#if 0
  fprintf(stderr, "evr: %s\n", pd->content);
#endif
  // intern and create
  return str2id(pool, pd->content, 1);
}


// create e:v-r from attributes
// atts is array of name,value pairs, NULL at end
//   even index into atts is name
//   odd index is value
//
static Id
evr_atts2id(Pool *pool, Parsedata *pd, const char **atts)
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
adddep(Pool *pool, Parsedata *pd, unsigned int olddeps, const char **atts, int isreq)
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
      else if (isreq && !strcmp(*a, "pre") && a[1][0] == '1')
        isreq = 2;
    }
  if (!n)			       /* quit if no name found */
    return olddeps;

  /* kind, name */
  if (k && !strcmp(k, "package"))
    k = NULL;			       /* package is default */

  if (k)			       /* if kind!=package, intern <kind>:<name> */
    {
      int l = strlen(k) + 1 + strlen(n) + 1;
      if (l > pd->acontent)	       /* extend buffer if needed */
	{
	  pd->content = (char *)realloc(pd->content, l + 256);
	  pd->acontent = l + 256;
	}
      sprintf(pd->content, "%s:%s", k, n);
      name = str2id(pool, pd->content, 1);
    }
  else
    name = str2id(pool, n, 1);       /* package: just intern <name> */

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
      id = rel2id(pool, name, evr, flags, 1);
    }
  else
    id = name;			       /* no operator */

  /* add new dependency to source */
  return source_addid_dep(pd->source, olddeps, id, isreq);
}


/*----------------------------------------------------------------*/

/*
 * XML callback
 * <name>
 * 
 */

static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  Parsedata *pd = (Parsedata *)userData;
  struct stateswitch *sw;
  Pool *pool = pd->pool;

  if (pd->depth != pd->statedepth)
    {
      pd->depth++;
      return;
    }

  /* ignore deps element */
  if (pd->state == STATE_PACKAGE && !strcmp(name, "deps"))
    return;

  pd->depth++;

  /* find node name in stateswitch */
  for (sw = pd->swtab[pd->state]; sw->from == pd->state; sw++)
  {
    if (!strcmp(sw->ename, name))
      break;
  }

  /* check if we're at the right level */
  if (sw->from != pd->state)
    {
#if 0
      fprintf(stderr, "into unknown: %s\n", name);
#endif
      return;
    }
  
  // set new state
  pd->state = sw->to;

  pd->docontent = sw->docontent;
  pd->statedepth = pd->depth;

  // start with empty content
  // (will collect data until end element
  pd->lcontent = 0;
  *pd->content = 0;

  switch (pd->state)
    {

    case STATE_NAME:
      if (pd->kind)		       /* if kind is set (non package) */
        {
          strcpy(pd->content, pd->kind);
          pd->lcontent = strlen(pd->content);
	  pd->content[pd->lcontent++] = ':';   /* prefix name with '<kind>:' */
	  pd->content[pd->lcontent] = 0;
	}
      break;

    case STATE_SUBCHANNEL:
      pd->pack = 0;
      break;

    case STATE_PACKAGE:		       /* solvable name */

      if ((pd->pack & PACK_BLOCK) == 0)  /* alloc new block ? */
	{
	  pool->solvables = (Solvable *)realloc(pool->solvables, (pool->nsolvables + pd->pack + PACK_BLOCK + 1) * sizeof(Solvable));
	  pd->start = pool->solvables + pd->source->start;
          memset(pd->start + pd->pack, 0, (PACK_BLOCK + 1) * sizeof(Solvable));
	  if (!pd->deps)
	    pd->deps = (Deps *)malloc((pd->pack + PACK_BLOCK + 1) * sizeof(Deps));
	  else
	    pd->deps = (Deps *)realloc(pd->deps, (pd->pack + PACK_BLOCK + 1) * sizeof(Deps));
          memset(pd->deps + pd->pack, 0, (PACK_BLOCK + 1) * sizeof(Deps));
	}

      if (!strcmp(name, "selection"))
        pd->kind = "selection";
      else if (!strcmp(name, "pattern"))
        pd->kind = "pattern";
      else if (!strcmp(name, "atom"))
        pd->kind = "atom";
      else if (!strcmp(name, "patch"))
        pd->kind = "patch";
      else
        pd->kind = NULL;	       /* default is package */
      pd->levrspace = 1;
      pd->epoch = 0;
      pd->version = 0;
      pd->release = 0;
#if 0
      fprintf(stderr, "package #%d\n", pd->pack);
#endif
      break;

    case STATE_UPDATE:
      pd->levrspace = 1;
      pd->epoch = 0;
      pd->version = 0;
      pd->release = 0;
      break;

    case STATE_PROVIDES:	       /* start of provides */
      pd->deps[pd->pack].provides = 0;
      break;
    case STATE_PROVIDESENTRY:	       /* entry within provides */
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


/*
 * XML callback
 * </name>
 * 
 * create Solvable from collected data
 */

static void XMLCALL
endElement(void *userData, const char *name)
{
  Parsedata *pd = (Parsedata *)userData;
  Pool *pool = pd->pool;
  Solvable *s = pd->start ? pd->start + pd->pack : NULL;
  Id evr;

  if (pd->depth != pd->statedepth)
    {
      pd->depth--;
      // printf("back from unknown %d %d %d\n", pd->state, pd->depth, pd->statedepth);
      return;
    }

  /* ignore deps element */
  if (pd->state == STATE_PACKAGE && !strcmp(name, "deps"))
    return;

  pd->depth--;
  pd->statedepth--;
  switch (pd->state)
    {

    case STATE_PACKAGE:		       /* package complete */

      if (!s->arch)                    /* default to "noarch" */
	s->arch = ARCH_NOARCH;

      if (!s->evr && pd->version)      /* set solvable evr */
        s->evr = evr2id(pool, pd,
                        pd->epoch   ? pd->evrspace + pd->epoch   : 0,
                        pd->version ? pd->evrspace + pd->version : 0,
                        pd->release ? pd->evrspace + pd->release : 0);
      /* ensure self-provides */
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        {
          pd->deps[pd->pack].provides = source_addid_dep(pd->source, pd->deps[pd->pack].provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
        }
      pd->pack++;		       /* inc pack count */
      break;
    case STATE_NAME:
      s->name = str2id(pool, pd->content, 1);
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
      if (!s->evr || evrcmp(pool, s->evr, evr) <= 0)
	s->evr = evr;
      break;
    case STATE_EPOCH:
    case STATE_VERSION:
    case STATE_RELEASE:
    case STATE_PEPOCH:
    case STATE_PVERSION:
    case STATE_PRELEASE:
      /* ensure buffer space */
      if (pd->lcontent + 1 + pd->levrspace > pd->aevrspace)
	{
	  pd->evrspace = (char *)realloc(pd->evrspace, pd->lcontent + 1 + pd->levrspace + 256);
	  pd->aevrspace = pd->lcontent + 1 + pd->levrspace + 256;
	}
      memcpy(pd->evrspace + pd->levrspace, pd->content, pd->lcontent + 1);
      if (pd->state == STATE_EPOCH || pd->state == STATE_PEPOCH)
	pd->epoch = pd->levrspace;
      else if (pd->state == STATE_VERSION || pd->state == STATE_PVERSION)
	pd->version = pd->levrspace;
      else
	pd->release = pd->levrspace;
      pd->levrspace += pd->lcontent + 1;
      break;
    case STATE_ARCH:
    case STATE_PARCH:
      s->arch = str2id(pool, pd->content, 1);
      break;
    default:
      break;
    }
  pd->state = pd->sbtab[pd->state];
  pd->docontent = 0;
  // printf("back from known %d %d %d\n", pd->state, pd->depth, pd->statedepth);
}


/*
 * XML callback
 * character data
 * 
 */

static void XMLCALL
characterData(void *userData, const XML_Char *s, int len)
{
  Parsedata *pd = (Parsedata *)userData;
  int l;
  char *c;

  // check if current nodes content is interesting
  if (!pd->docontent)
    return;

  // adapt content buffer
  l = pd->lcontent + len + 1;
  if (l > pd->acontent)
    {
      pd->content = (char *)realloc(pd->content, l + 256);
      pd->acontent = l + 256;
    }
  // append new content to buffer
  c = pd->content + pd->lcontent;
  pd->lcontent += len;
  while (len-- > 0)
    *c++ = *s++;
  *c = 0;
}

/*-------------------------------------------------------------------*/

#define BUFF_SIZE 8192

/*
 * read 'helix' type xml from fp
 * add packages to pool/source
 * 
 */

Source *
pool_addsource_helix(Pool *pool, FILE *fp)
{
  Parsedata pd;
  char buf[BUFF_SIZE];
  int i, l;
  Source *source;
  Solvable *solvable;
  Deps *deps;
  struct stateswitch *sw;

  // create empty source
  source = pool_addsource_empty(pool);

  // prepare parsedata
  memset(&pd, 0, sizeof(pd));
  for (i = 0, sw = stateswitches; sw->from != NUMSTATES; i++, sw++)
    {
      if (!pd.swtab[sw->from])
        pd.swtab[sw->from] = sw;
      pd.sbtab[sw->to] = sw->from;
    }

  pd.pool = pool;
  pd.source = source;

  pd.content = (char *)malloc(256);	/* must hold all solvable kinds! */
  pd.acontent = 256;
  pd.lcontent = 0;

  pd.evrspace = (char *)malloc(256);
  pd.aevrspace= 256;
  pd.levrspace = 1;

  // set up XML parser

  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &pd);       /* make parserdata available to XML callbacks */
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // read/parse XML file
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

  // adapt package count
  pool->nsolvables += pd.pack;
  source->nsolvables = pd.pack;

  // now set dependency pointers for each solvable
  deps = pd.deps;
  solvable = pool->solvables + source->start;
  for (i = 0; i < pd.pack; i++, solvable++)
    {
      if (deps[i].provides)
        solvable->provides = source->idarraydata + deps[i].provides;
      if (deps[i].requires)
        solvable->requires = source->idarraydata + deps[i].requires;
      if (deps[i].conflicts)
        solvable->conflicts = source->idarraydata + deps[i].conflicts;
      if (deps[i].obsoletes)
        solvable->obsoletes = source->idarraydata + deps[i].obsoletes;
      if (deps[i].recommends)
        solvable->recommends = source->idarraydata + deps[i].recommends;
      if (deps[i].supplements)
        solvable->supplements = source->idarraydata + deps[i].supplements;
      if (deps[i].suggests)
        solvable->suggests = source->idarraydata + deps[i].suggests;
      if (deps[i].enhances)
        solvable->enhances = source->idarraydata + deps[i].enhances;
      if (deps[i].freshens)
        solvable->freshens = source->idarraydata + deps[i].freshens;
    }

  free(deps);
  free(pd.content);
  free(pd.evrspace);

  return source;
}
