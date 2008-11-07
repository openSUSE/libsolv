/*
 * repo_zyppdb.c
 *
 * Parses /var/lib/zypp/db/products/...
 * The are old (pre Code11) products. See bnc#429177
 *
 *
 * Copyright (c) 2008, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>
#include <expat.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#define DISABLE_SPLIT
#include "tools_util.h"
#include "repo_content.h"


//#define DUMPOUT 0

enum state {
  STATE_START,           // 0
  STATE_PRODUCT,         // 1
  STATE_NAME,            // 2
  STATE_VERSION,         // 3
  STATE_ARCH,            // 4
  STATE_SUMMARY,         // 5
  STATE_VENDOR,          // 6
  STATE_INSTALLTIME,     // 7
  NUMSTATES              // 0
};

struct stateswitch {
  enum state from;
  char *ename;
  enum state to;
  int docontent;
};

/* !! must be sorted by first column !! */
static struct stateswitch stateswitches[] = {
  { STATE_START,     "product",       STATE_PRODUCT,       0 },
  { STATE_PRODUCT,   "name",          STATE_NAME,          1 },
  { STATE_PRODUCT,   "version",       STATE_VERSION,       0 },
  { STATE_PRODUCT,   "arch",          STATE_ARCH,          1 },
  { STATE_PRODUCT,   "summary",       STATE_SUMMARY,       1 },
  { STATE_PRODUCT,   "install-time",  STATE_INSTALLTIME,   1 },
  { STATE_PRODUCT,   "vendor",        STATE_VENDOR,        1 },
  { NUMSTATES }
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

  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];

  const char *tmplang;

  Solvable *solvable;
  Id handle;

  Id langcache[ID_NUM_INTERNAL];
};


/*
 * find_attr
 * find value for xml attribute
 * I: txt, name of attribute
 * I: atts, list of key/value attributes
 * I: dup, strdup it
 * O: pointer to value of matching key, or NULL
 *
 */

static inline const char *
find_attr(const char *txt, const char **atts, int dup)
{
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, txt))
        return dup ? strdup(atts[1]) : atts[1];
    }
  return 0;
}


/*
 * create localized tag
 */

static Id
langtag(struct parsedata *pd, Id tag, const char *language)
{
  if (language && !language[0])
    language = 0;
  if (!language || tag >= ID_NUM_INTERNAL)
    return pool_id2langid(pd->repo->pool, tag, language, 1);
  if (!pd->langcache[tag])
    pd->langcache[tag] = pool_id2langid(pd->repo->pool, tag, language, 1);
  return pd->langcache[tag];
}


/*
 * XML callback: startElement
 */

static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  struct stateswitch *sw;

#if 0
      fprintf(stderr, "start: [%d]%s\n", pd->state, name);
#endif
  if (pd->depth != pd->statedepth)
    {
      pd->depth++;
      return;
    }

  pd->depth++;
  if (!pd->swtab[pd->state])	/* no statetable -> no substates */
    {
#if 0
      fprintf(stderr, "into unknown: %s (from: %d)\n", name, pd->state);
#endif
      return;
    }
  for (sw = pd->swtab[pd->state]; sw->from == pd->state; sw++)  /* find name in statetable */
    if (!strcmp(sw->ename, name))
      break;

  if (sw->from != pd->state)
    {
#if 0
      fprintf(stderr, "into unknown: %s (from: %d)\n", name, pd->state);
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
    case STATE_PRODUCT:
      {
	/* parse 'type' */
	const char *type = find_attr("type", atts, 0);
	s = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
	repodata_extend(pd->data, s - pool->solvables);
	pd->handle = s - pool->solvables;
	if (type)
	  {
	    repodata_set_str(pd->data, pd->handle, PRODUCT_TYPE, type);
	  }
      }
      break;
    case STATE_VERSION:
      {
	const char *ver = find_attr("ver", atts, 0);
	const char *rel = find_attr("rel", atts, 0);
	/* const char *epoch = find_attr("epoch", atts, 1); ignored */
	s->evr = makeevr(pd->pool, join2(ver, "-", rel));
      }
      break;
      /* <summary lang="xy">... */
    case STATE_SUMMARY:
      pd->tmplang = find_attr("lang", atts, 1);
      break;
    default:
      break;
    }
}


static void XMLCALL
endElement(void *userData, const char *name)
{
  struct parsedata *pd = userData;
  Solvable *s = pd->solvable;

#if 0
      fprintf(stderr, "end: [%d]%s\n", pd->state, name);
#endif
  if (pd->depth != pd->statedepth)
    {
      pd->depth--;
#if 0
      fprintf(stderr, "back from unknown %d %d %d\n", pd->state, pd->depth, pd->statedepth);
#endif
      return;
    }

  pd->depth--;
  pd->statedepth--;

  switch (pd->state)
    {
    case STATE_PRODUCT:

      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->provides = repo_addid_dep(pd->repo, s->provides, rel2id(pd->pool, s->name, s->evr, REL_EQ, 1), 0);
      pd->solvable = 0;
      break;
    case STATE_NAME:
      s->name = str2id(pd->pool, join2("product", ":", pd->content), 1);
      break;
    case STATE_ARCH:
      s->arch = str2id(pd->pool, pd->content, 1);
      break;
    case STATE_SUMMARY:
      repodata_set_str(pd->data, pd->handle, langtag(pd, SOLVABLE_SUMMARY, pd->tmplang), pd->content);
      pd->tmplang = sat_free((void *)pd->tmplang);
      break;
    case STATE_VENDOR:
      s->vendor = str2id(pd->pool, pd->content, 1);
      break;
    case STATE_INSTALLTIME:
      repodata_set_num(pd->data, pd->handle, SOLVABLE_INSTALLTIME, atol(pd->content));
    default:
      break;
    }

  pd->state = pd->sbtab[pd->state];
  pd->docontent = 0;

#if 0
      fprintf(stderr, "end: [%s] -> %d\n", name, pd->state);
#endif
}


static void XMLCALL
characterData(void *userData, const XML_Char *s, int len)
{
  struct parsedata *pd = userData;
  int l;
  char *c;
  if (!pd->docontent) {
#if 0
    char *dup = strndup( s, len );
  fprintf(stderr, "Content: [%d]'%s'\n", pd->state, dup );
  free( dup );
#endif
    return;
  }
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


/*
 * add single product to repo
 *
 */

static void
repo_add_product(struct parsedata *pd, Repodata *data, FILE *fp)
{
  char buf[BUFF_SIZE];
  int l;

  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, pd);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  fprintf(stderr, "repo_zyppdb: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
	  return;
	}
      if (l == 0)
	break;
    }
  XML_ParserFree(parser);
  return;
}



/*
 * parse dir for products
 */

static void
parse_dir(DIR *dir, const char *path, struct parsedata *pd, Repodata *repodata)
{
  struct dirent *entry;

  while ((entry = readdir(dir)))
    {
      int len = strlen(entry->d_name);
      if (len < 3)   /* skip '.' and '..' */
	continue;
      char *fullpath = join2(path, "/", entry->d_name);
      FILE *fp = fopen(fullpath, "r");
      if (!fp)
	{
	  perror(fullpath);
	  break;
	}
      repo_add_product(pd, repodata, fp);
      fclose(fp);
    }
}


/*
 * read all installed products
 *
 * parse each one as a product
 */

void
repo_add_zyppdb_products(Repo *repo, Repodata *repodata, const char *fullpath, DIR *dir)
{
  int i;
  struct parsedata pd;
  struct stateswitch *sw;

  memset(&pd, 0, sizeof(pd));
  pd.repo = repo;
  pd.pool = repo->pool;
  pd.data = repodata;

  pd.content = malloc(256);
  pd.acontent = 256;

  for (i = 0, sw = stateswitches; sw->from != NUMSTATES; i++, sw++)
    {
      if (!pd.swtab[sw->from])
        pd.swtab[sw->from] = sw;
      pd.sbtab[sw->to] = sw->from;
    }

  parse_dir(dir, fullpath, &pd, repodata);

  sat_free((void *)pd.tmplang);
  free(pd.content);
  join_freemem();
}

/* EOF */
