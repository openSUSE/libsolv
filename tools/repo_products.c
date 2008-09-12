/*
 * repo_products.c
 *
 * Parses all files below 'proddir'
 * See http://en.opensuse.org/Product_Management/Code11
 *
 *
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
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


static ino_t baseproduct = 0;
static ino_t currentproduct = 0;

static int productscheme = -1;

//#define DUMPOUT 0

enum state {
  STATE_START,           // 0
  STATE_PRODUCT,         // 1
  STATE_VENDOR,          // 2
  STATE_NAME,            // 3
  STATE_VERSION,         // 4
  STATE_RELEASE,         // 5
  STATE_ARCH,            // 6
  STATE_SUMMARY,         // 7
  STATE_DESCRIPTION,     // 8
  STATE_UPDATEREPOKEY,   // 9
  STATE_URLS,            // 10
  STATE_URL,             // 11
#if 0 /* not needed */
  STATE_BUILDCONFIG,     // 12
  STATE_INSTALLCONFIG,   // 13
#endif
  STATE_RUNTIMECONFIG,   // 14
  STATE_LINGUAS,         // 15
  STATE_LANG,            // 16
  STATE_REGISTER,        // 17
  STATE_TARGET,          // 18
  STATE_FLAVOR,          // 19
  STATE_REGRELEASE,      // 20
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
  { STATE_PRODUCT,   "vendor",        STATE_VENDOR,        1 },
  { STATE_PRODUCT,   "name",          STATE_NAME,          1 },
  { STATE_PRODUCT,   "version",       STATE_VERSION,       1 },
  { STATE_PRODUCT,   "release",       STATE_RELEASE,       1 },
  { STATE_PRODUCT,   "arch",          STATE_ARCH,          1 },
  { STATE_PRODUCT,   "summary",       STATE_SUMMARY,       1 },
  { STATE_PRODUCT,   "description",   STATE_DESCRIPTION,   1 },
  { STATE_PRODUCT,   "register",      STATE_REGISTER,      0 },
  { STATE_PRODUCT,   "urls",          STATE_URLS,          0 },
  { STATE_PRODUCT,   "runtimeconfig", STATE_RUNTIMECONFIG, 0 },
#if 0 /* not needed */
  { STATE_PRODUCT,   "installconfig", STATE_INSTALLCONFIG, 0 },
  { STATE_PRODUCT,   "buildconfig",   STATE_BUILDCONFIG,   0 },
#endif
  { STATE_PRODUCT,   "linguas",       STATE_LINGUAS,       0 },
  { STATE_PRODUCT,   "updaterepokey", STATE_UPDATEREPOKEY, 1 },
  { STATE_URLS,      "url",           STATE_URL,           1 },
/*  { STATE_BUILDCONFIG,"linguas",      STATE_LINGUAS,       0 }, */
  { STATE_LINGUAS,   "lang",          STATE_LANG,          0 },
  { STATE_REGISTER,  "flavor",        STATE_FLAVOR,        1 },
  { STATE_REGISTER,  "target",        STATE_TARGET,        1 },
  { STATE_REGISTER,  "release",       STATE_REGRELEASE,    1 },
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
  int datanum;

  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
  const char *attribute; /* only print this attribute, if currentproduct == baseproduct */

  const char *tmplang;
  const char *tmpvers;
  const char *tmprel;
  const char *tmpurltype;

  Solvable *s;
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
  if ( !(sw = pd->swtab[pd->state]) ) /* no statetable -> no substates */
    {
#if 0
      fprintf(stderr, "into unknown: [?]%s (from: ?, state %d)\n", name, pd->state);
#endif
      return;
    }
  for (; sw->from == pd->state; sw++)  /* find name in statetable */
    if (!strcmp(sw->ename, name))
      break;

  if (sw->from != pd->state)
    {
#if 0
      fprintf(stderr, "into unknown: [%d]%s (from: %d, state %d)\n", sw->to, name, sw->from, pd->state);
      exit( 1 );
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
      /* parse 'schemeversion' and store in global variable */
      {
        const char * scheme = find_attr("schemeversion", atts, 0);
        productscheme = (scheme && *scheme) ? atoi(scheme) : -1;
      }
      if (!pd->s)
	{
	  pd->s = pool_id2solvable(pool, repo_add_solvable(pd->repo));
	  repodata_extend(pd->data, pd->s - pool->solvables);
	  pd->handle = repodata_get_handle(pd->data, pd->s - pool->solvables - pd->repo->start);
	}
     break;

      /* <summary lang="xy">... */
    case STATE_SUMMARY:
      pd->tmplang = find_attr("lang", atts, 1);
      break;
    case STATE_DESCRIPTION:
      pd->tmplang = find_attr("lang", atts, 1);
      break;
    case STATE_URL:
      pd->tmpurltype = find_attr("name", atts, 1);
      break;
    default:
      break;
    }
  return;
}


static void XMLCALL
endElement(void *userData, const char *name)
{
  struct parsedata *pd = userData;

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
    case STATE_VENDOR:
      pd->s->vendor = str2id(pd->pool, pd->content, 1);
      break;
    case STATE_NAME:
      pd->s->name = str2id(pd->pool, join2("product", ":", pd->content), 1);
      break;
    case STATE_VERSION:
      pd->tmpvers = strdup(pd->content);
      break;
    case STATE_RELEASE:
      pd->tmprel = strdup(pd->content);
      break;
    case STATE_ARCH:
      pd->s->arch = str2id(pd->pool, pd->content, 1);
      break;
    case STATE_UPDATEREPOKEY:
      repodata_set_str(pd->data, pd->handle, langtag(pd, PRODUCT_UPDATEREPOKEY, pd->tmplang), pd->content);
      break;
    case STATE_SUMMARY:
      repodata_set_str(pd->data, pd->handle, langtag(pd, SOLVABLE_SUMMARY, pd->tmplang), pd->content);
      if (pd->tmplang)
      {
        free( (char *)pd->tmplang );
	pd->tmplang = 0;
      }
      break;
    case STATE_DESCRIPTION:
      repodata_set_str(pd->data, pd->handle, langtag(pd, SOLVABLE_DESCRIPTION, pd->tmplang), pd->content );
      if (pd->tmplang)
      {
        free( (char *)pd->tmplang );
	pd->tmplang = 0;
      }
      break;
    case STATE_URL:
      if (pd->tmpurltype)
        {
          repodata_add_poolstr_array(pd->data, pd->handle, PRODUCT_URL, pd->content);

          repodata_add_idarray(pd->data, pd->handle, PRODUCT_URL_TYPE, str2id(pd->pool, pd->tmpurltype, 1));
        }
      break;
    case STATE_TARGET:
      if (currentproduct == baseproduct
	  && pd->attribute
	  && !strcmp(pd->attribute, "register.target"))
	{
	  printf("%s\n", pd->content);
	}
    break;
    case STATE_FLAVOR:
      repodata_set_str(pd->data, pd->handle, PRODUCT_FLAVOR, pd->content );
      if (currentproduct == baseproduct
	  && pd->attribute
	  && !strcmp(pd->attribute, "register.flavor"))
	{
	  printf("%s\n", pd->content);
	}
    break;
    case STATE_REGRELEASE:
      if (currentproduct == baseproduct
	  && pd->attribute
	  && !strcmp(pd->attribute, "register.release"))
	{
	  printf("%s\n", pd->content);
	}
    break;
    default:
      break;
    }

  pd->state = pd->sbtab[pd->state];
  pd->docontent = 0;

#if 0
      fprintf(stderr, "end: [%s] -> %d\n", name, pd->state);
#endif
  return;
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
repo_add_product(struct parsedata *pd, Repodata *data, FILE *fp, int code11)
{
  Pool *pool = pd->pool;
  char buf[BUFF_SIZE];
  int i, l;
  struct stateswitch *sw;
  struct stat st;

  pd->s = NULL; /* enforce new solvable when coming here again */

  if (!fstat(fileno(fp), &st))
    currentproduct = st.st_ino;
  else
    {
      currentproduct = baseproduct+1; /* make it != baseproduct if stat fails */
      st.st_ctime = 0;
      perror("Can't stat()");
    }

  for (i = 0, sw = stateswitches; sw->from != NUMSTATES; i++, sw++)
    {
      if (!pd->swtab[sw->from])
        pd->swtab[sw->from] = sw;
      pd->sbtab[sw->to] = sw->from;
    }
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, pd);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  fprintf(stderr, "repo_products: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
	  exit(1);
	}
      if (l == 0)
	break;
    }
  XML_ParserFree(parser);

  if (pd->s)
    {
      Solvable *s = pd->s;

      if (st.st_ctime)
        repodata_set_num(pd->data, pd->handle, SOLVABLE_INSTALLTIME, st.st_ctime);
      /* this is where <productsdir>/baseproduct points to */
      if (currentproduct == baseproduct)
	repodata_set_str(pd->data, pd->handle, PRODUCT_TYPE, "base");

      if (pd->tmprel)
	{
	  if (pd->tmpvers)
	    {
	      s->evr = makeevr(pool, join2(pd->tmpvers, "-", pd->tmprel));
	      free((char *)pd->tmpvers);
	      pd->tmpvers = 0;
	    }
	  else
	    {
	      fprintf(stderr, "Seen <release> but no <version>\n");
	    }
	  free((char *)pd->tmprel);
	  pd->tmprel = 0;
	}
      else if (pd->tmpvers)
	{
	  s->evr = makeevr(pool, pd->tmpvers); /* just version, no release */
	  free((char *)pd->tmpvers);
	  pd->tmpvers = 0;
	}
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	{
	  s->provides = repo_addid_dep(pd->repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	}
    } /* if pd->s */

  return;
}


/*
 * parse dir looking for files ending in suffix
 */

static void
parse_dir(DIR *dir, const char *path, struct parsedata *pd, Repodata *repodata, int code11)
{
  struct dirent *entry;
  char *suffix = code11 ? ".prod" : "-release";
  int slen = code11 ? 5 : 8;  /* strlen(".prod") : strlen("-release") */
  struct stat st;

  /* check for <productsdir>/baseproduct on code11 and remember its target inode */
  if (code11
      && stat(join2(path, "/", "baseproduct"), &st) == 0) /* follow symlink */
    {
      baseproduct = st.st_ino;
    }
  else
    baseproduct = 0;

  while ((entry = readdir(dir)))
    {
      int len;
      len = strlen(entry->d_name);

      /* skip /etc/lsb-release, thats not a product per-se */
      if (!code11
	  && strcmp(entry->d_name, "lsb-release") == 0)
	{
	  continue;
	}

      if (len > slen
	  && strcmp(entry->d_name+len-slen, suffix) == 0)
	{
	  char *fullpath = join2(path, "/", entry->d_name);
	  FILE *fp = fopen(fullpath, "r");
	  if (!fp)
	    {
	      perror(fullpath);
	      break;
	    }
	  repo_add_product(pd, repodata, fp, code11);
	  fclose(fp);
	}
    }
}


/*
 * read all installed products
 *
 * try proddir (reading all .xml files from this directory) first
 * if not available, assume non-code11 layout and parse /etc/xyz-release
 *
 * parse each one as a product
 */

void
repo_add_products(Repo *repo, Repodata *repodata, const char *proddir, const char *root, const char *attribute)
{
  const char *fullpath = proddir;
  int code11 = 1;
  DIR *dir = opendir(fullpath);
  struct parsedata pd;

  memset(&pd, 0, sizeof(pd));
  pd.repo = repo;
  pd.pool = repo->pool;
  pd.data = repodata;

  pd.content = malloc(256);
  pd.acontent = 256;

  pd.attribute = attribute;

  if (!dir)
    {
      fullpath = root ? join2(root, "", "/etc") : "/etc";
      dir = opendir(fullpath);
      code11 = 0;
    }
  if (!dir)
    {
      perror(fullpath);
    }
  else
    {
      parse_dir(dir, fullpath, &pd, repodata, code11);
    }

  free(pd.content);
  join_freemem();
  closedir(dir);
}

/* EOF */
