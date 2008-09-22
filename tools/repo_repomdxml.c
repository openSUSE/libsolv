/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define DO_ARRAY 1

#define _GNU_SOURCE
#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

#include "pool.h"
#include "repo.h"
#include "repo_updateinfoxml.h"

//#define DUMPOUT 0

/*
<repomd>
<data type="primary">
<location href="repodata/primary.xml.gz"/>
<checksum type="sha">e9162516fa25fec8d60caaf4682d2e49967786cc</checksum>
<timestamp>1215708444</timestamp>
<open-checksum type="sha">c796c48184cd5abc260e4ba929bdf01be14778a7</open-checksum>
</data>
<data type="filelists">
<location href="repodata/filelists.xml.gz"/>
<checksum type="sha">1c638295c49e9707c22810004ebb0799791fcf45</checksum>
<timestamp>1215708445</timestamp>
<open-checksum type="sha">54a40d5db3df0813b8acbe58cea616987eb9dc16</open-checksum>
</data>
<data type="other">
<location href="repodata/other.xml.gz"/>
<checksum type="sha">a81ef39eaa70e56048f8351055119d8c82af2491</checksum>
<timestamp>1215708447</timestamp>
<open-checksum type="sha">4d1ee867c8864025575a2fb8fde3b85371d51978</open-checksum>
</data>
<data type="deltainfo">
<location href="repodata/deltainfo.xml.gz"/>
<checksum type="sha">5880cfa5187026a24a552d3c0650904a44908c28</checksum>
<timestamp>1215708447</timestamp>
<open-checksum type="sha">7c964a2c3b17df5bfdd962c3be952c9ca6978d8b</open-checksum>
</data>
<data type="updateinfo">
<location href="repodata/updateinfo.xml.gz"/>
<checksum type="sha">4097f7e25c7bb0770ae31b2471a9c8c077ee904b</checksum>
<timestamp>1215708447</timestamp>
<open-checksum type="sha">24f8252f3dd041e37e7c3feb2d57e02b4422d316</open-checksum>
</data>
<data type="diskusage">
<location href="repodata/diskusage.xml.gz"/>
<checksum type="sha">4097f7e25c7bb0770ae31b2471a9c8c077ee904b</checksum>
<timestamp>1215708447</timestamp>
<open-checksum type="sha">24f8252f3dd041e37e7c3feb2d57e02b4422d316</open-checksum>
</data>
</repomd>

support also extension suseinfo format
<suseinfo>
  <expire>timestamp</expire>
  <products>
    <id>...</id>
  </products>
  <kewwords>
    <k>...</k>
  </keywords>
</suseinfo>

*/

enum state {
  STATE_START,
  /* extension tags */
  STATE_SUSEINFO,
  STATE_EXPIRE,
  STATE_PRODUCTS,
  STATE_PRODUCT,
  STATE_KEYWORDS,
  STATE_KEYWORD,
  /* normal repomd.xml */
  STATE_REPOMD,
  STATE_DATA,
  STATE_LOCATION,
  STATE_CHECKSUM,
  STATE_TIMESTAMP,
  STATE_OPENCHECKSUM,
  NUMSTATES
};

struct stateswitch {
  enum state from;
  char *ename;
  enum state to;
  int docontent;
};

/* !! must be sorted by first column !! */
static struct stateswitch stateswitches[] = {
  /* suseinfo tags */
  { STATE_START,       "repomd",          STATE_REPOMD, 0 },
  { STATE_START,       "suseinfo",        STATE_SUSEINFO, 0 },  
  { STATE_SUSEINFO,    "expire",          STATE_EXPIRE, 1 },  
  { STATE_SUSEINFO,    "products",        STATE_PRODUCTS, 0 },  
  { STATE_SUSEINFO,    "keywords",        STATE_KEYWORDS, 0 },  
  { STATE_PRODUCTS,    "id",              STATE_PRODUCT, 1 },  
  { STATE_KEYWORDS,    "k",               STATE_KEYWORD, 1 },  
  /* standard tags */
  { STATE_REPOMD,      "data",            STATE_DATA,  0 },
  { STATE_DATA,        "location",        STATE_LOCATION, 0 },
  { STATE_DATA,        "checksum",        STATE_CHECKSUM, 1 },  
  { STATE_DATA,        "timestamp",       STATE_TIMESTAMP, 1 },
  { STATE_DATA,        "open-checksum",    STATE_OPENCHECKSUM, 1 },
  { NUMSTATES }
};

/*
 * split l into m parts, store to sp[]
 *  split at whitespace
 */

static inline int
split_comma(char *l, char **sp, int m)
{
  int i;
  for (i = 0; i < m;)
    {
      while (*l == ',')
	l++;
      if (!*l)
	break;
      sp[i++] = l;
      if (i == m)
        break;
      while (*l && !(*l == ','))
	l++;
      if (!*l)
	break;
      *l++ = 0;
    }
  return i;
}


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
  int timestamp;
};

/*
 * find attribute
 */

static inline const char *
find_attr(const char *txt, const char **atts)
{
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, txt))
        return atts[1];
    }
  return 0;
}


static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  /*Pool *pool = pd->pool;*/
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
  if (!pd->swtab[pd->state])
    return;
  for (sw = pd->swtab[pd->state]; sw->from == pd->state; sw++)  /* find name in statetable */
    if (!strcmp(sw->ename, name))
      break;
  
  if (sw->from != pd->state)
    {
#if 1
      fprintf(stderr, "into unknown: %s (from: %d)\n", name, pd->state);
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
    case STATE_START: break;
    case STATE_REPOMD:
      {
        const char *updstr;
        char *value;
        char *fvalue;

        /* this should be OBSOLETE soon */
        updstr = find_attr("updates", atts);
        if ( updstr != NULL )
          {
            value = strdup(updstr);
            fvalue = value; /* save the first */
            if ( value != NULL )
              {
                char *sp[2];
                while (value)
                  {
                    int words = split_comma(value, sp, 2);
                    if (!words)
                      break;
                    if (sp[0])
                      repo_add_poolstr_array(pd->repo, -1, REPOSITORY_UPDATES, sp[0]);
                    if (words == 1)
                      break;
                    value = sp[1];
                  }
                free(fvalue);
              }
          }
          break;
        }
    case STATE_SUSEINFO: break;
    case STATE_EXPIRE: break;
    case STATE_PRODUCTS: break;
    case STATE_PRODUCT: break;
    case STATE_KEYWORDS: break;
    case STATE_KEYWORD: break;
    case STATE_DATA: break;
    case STATE_LOCATION: break;
    case STATE_CHECKSUM: break;
    case STATE_TIMESTAMP: break;
    case STATE_OPENCHECKSUM: break;
    case NUMSTATES: break;
    default: break;
    }
  return;
}

static void XMLCALL
endElement(void *userData, const char *name)
{
  struct parsedata *pd = userData;
  /* Pool *pool = pd->pool; */
  int timestamp;

#if 0
      fprintf(stderr, "end: %s\n", name);
#endif
  if (pd->depth != pd->statedepth)
    {
      pd->depth--;
#if 1
      fprintf(stderr, "back from unknown %d %d %d\n", pd->state, pd->depth, pd->statedepth);
#endif
      return;
    }

  pd->depth--;
  pd->statedepth--;
  switch (pd->state)
    {
    case STATE_START: break;
    case STATE_REPOMD: 
      /* save the timestamp in the non solvable number 1 */
      if ( pd->timestamp > 0 )
        repo_set_num(pd->repo, -1, REPOSITORY_TIMESTAMP, pd->timestamp);
      break;
    case STATE_DATA: break;
    case STATE_LOCATION: break;
    case STATE_CHECKSUM: break;
    case STATE_OPENCHECKSUM: break;
    case STATE_TIMESTAMP:
      {
        /**
         * we want to look for the newer timestamp
         * of all resources to save it as the time
         * the metadata was generated
         */
        timestamp = atoi(pd->content);
        /** if the timestamp is invalid or just 0 ignore it */
        if ( timestamp == 0 )
          break;
        if ( timestamp > pd->timestamp )
          {
            pd->timestamp = timestamp;
          }
        break;
      }
    case STATE_EXPIRE:
      {
        int expire = 0;
        if ( pd->content )
          {
            expire = atoi(pd->content);
            if ( expire > 0 )
              {
                /* save the timestamp in the non solvable number 1 */
                repo_set_num(pd->repo, -1, REPOSITORY_EXPIRE, expire);
              }
          }
        break;
      }
    case STATE_PRODUCT:
      {
        if ( pd->content )
          repo_add_poolstr_array(pd->repo, -1, REPOSITORY_PRODUCTS, pd->content);
        break;
      }
    case STATE_KEYWORD:
      {
        if ( pd->content )
          repo_add_poolstr_array(pd->repo, -1, REPOSITORY_KEYWORDS, pd->content);
        break;
      }
    case STATE_SUSEINFO: break;
    case STATE_PRODUCTS: break;
    case STATE_KEYWORDS: break;
    case NUMSTATES: break;              
    default:
      break;
    }

  pd->state = pd->sbtab[pd->state];
  pd->docontent = 0;
  
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

void
repo_add_repomdxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  pd.timestamp = 0;

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
  pd.data = repo_add_repodata(pd.repo, 0);

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
	  fprintf(stderr, "repo_repomdxml: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
	  exit(1);
	}
      if (l == 0)
	break;
    }
  XML_ParserFree(parser);

  if (pd.data)
    repodata_internalize(pd.data);

  free(pd.content);
}

/* EOF */
