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
#include "chksum.h"
#include "repo_updateinfoxml.h"

//#define DUMPOUT 0

/*
<repomd>

  <!-- these tags are available in create repo > 0.9.6 -->
  <revision>timestamp_or_arbitrary_user_supplied_string</revision>
  <tags>
    <content>opensuse</content>
    <content>i386</content>
    <content>other string</content>
    <distro cpeid="cpe://o:opensuse_project:opensuse:11">openSUSE 11.0</distro>
  </tags>
  <!-- end -->

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
  STATE_KEYWORDS,
  STATE_KEYWORD,

  /* normal repomd.xml */
  STATE_REPOMD,
  STATE_REVISION,
  STATE_TAGS,
  STATE_REPO,
  STATE_CONTENT,
  STATE_DISTRO,
  STATE_UPDATES,
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
  /* we support the tags element in suseinfo in case
     createrepo version does not support it yet */
  { STATE_SUSEINFO,    "tags",            STATE_TAGS, 0 },
  { STATE_SUSEINFO,    "expire",          STATE_EXPIRE, 1 },
  { STATE_SUSEINFO,    "keywords",        STATE_KEYWORDS, 0 },
  /* keywords is the suse extension equivalent of
     tags/content when this one was not yet available.
     therefore we parse both */
  { STATE_KEYWORDS,    "k",               STATE_KEYWORD, 1 },
  /* standard tags */
  { STATE_REPOMD,      "revision",        STATE_REVISION, 1 },
  { STATE_REPOMD,      "tags",            STATE_TAGS,  0 },
  { STATE_REPOMD,      "data",            STATE_DATA,  0 },

  { STATE_TAGS,        "repo",            STATE_REPO,    1 },
  { STATE_TAGS,        "content",         STATE_CONTENT, 1 },
  { STATE_TAGS,        "distro",          STATE_DISTRO,  1 },
  /* this tag is only valid in suseinfo.xml for now */
  { STATE_TAGS,        "updates",         STATE_UPDATES,  1 },

  { STATE_DATA,        "location",        STATE_LOCATION, 0 },
  { STATE_DATA,        "checksum",        STATE_CHECKSUM, 1 },
  { STATE_DATA,        "timestamp",       STATE_TIMESTAMP, 1 },
  { STATE_DATA,        "open-checksum",    STATE_OPENCHECKSUM, 1 },
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

  XML_Parser *parser;
  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
  int timestamp;
  /* handles for collection
     structures */
  /* repo updates */
  Id ruhandle;
  /* repo products */
  Id rphandle;
  /* repo data handle */
  Id rdhandle;

  const char *tmpattr;
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
    case STATE_START: break;
    case STATE_REPOMD:
      {
        const char *updstr;

        /* this should be OBSOLETE soon */
        updstr = find_attr("updates", atts);
        if (updstr)
          {
            char *value = strdup(updstr);
            char *fvalue = value; /* save the first */
            while (value)
	      {
		char *p = strchr(value, ',');
		if (*p)
		  *p++ = 0;
		if (*value)
		  repo_add_poolstr_array(pd->repo, SOLVID_META, REPOSITORY_UPDATES, value);
		value = p;
	      }
	    free(fvalue);
          }
          break;
        }
    case STATE_SUSEINFO: break;
    case STATE_EXPIRE: break;
    case STATE_KEYWORDS: break;
    case STATE_KEYWORD: break;
    case STATE_CONTENT: break;
    case STATE_REVISION: break;
    case STATE_DISTRO:
      {
        /* this is extra metadata about the product this repository
           was designed for */
        const char *cpeid = find_attr("cpeid", atts);
        pd->rphandle = repodata_new_handle(pd->data);
        /* set the cpeid for the product
           the label is set in the content of the tag */
        if (cpeid)
          repodata_set_poolstr(pd->data, pd->rphandle, REPOSITORY_PRODUCT_CPEID, cpeid);
        break;
      }
    case STATE_UPDATES:
      {
        /* this is extra metadata about the product this repository
           was designed for */
        const char *cpeid = find_attr("cpeid", atts);
        pd->ruhandle = repodata_new_handle(pd->data);
        /* set the cpeid for the product
           the label is set in the content of the tag */
        if (cpeid)
          repodata_set_poolstr(pd->data, pd->ruhandle, REPOSITORY_PRODUCT_CPEID, cpeid);
        break;
      }
    case STATE_DATA:
      {
        const char *type= find_attr("type", atts);
        pd->rdhandle = repodata_new_handle(pd->data);
	if (type)
          repodata_set_poolstr(pd->data, pd->rdhandle, REPOSITORY_REPOMD_TYPE, type);
        break;
      }
    case STATE_LOCATION:
      {
        const char *href = find_attr("href", atts);
	if (href)
          repodata_set_str(pd->data, pd->rdhandle, REPOSITORY_REPOMD_LOCATION, href);
      }
    case STATE_CHECKSUM:
    case STATE_OPENCHECKSUM:
      pd->tmpattr= find_attr("type", atts);
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
  /* Pool *pool = pd->pool; */

#if 0
  fprintf(stderr, "endElement: %s\n", name);
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
    case STATE_START: break;
    case STATE_REPOMD:
      if (pd->timestamp > 0)
        repodata_set_num(pd->data, SOLVID_META, REPOSITORY_TIMESTAMP, pd->timestamp);
      break;
    case STATE_DATA:
      if (pd->rdhandle)
        repodata_add_flexarray(pd->data, SOLVID_META, REPOSITORY_REPOMD, pd->rdhandle);
      pd->rdhandle = 0;
      break;
    case STATE_LOCATION: break;

    case STATE_CHECKSUM:
    case STATE_OPENCHECKSUM:
      {
        Id type = sat_chksum_str2type(pd->tmpattr);
	if (!type)
	  {
            fprintf(stderr, "Unknown checksum type: %d: %s\n", (unsigned int)XML_GetCurrentLineNumber(*pd->parser), pd->tmpattr);
            exit(1);
	  }
	if (strlen(pd->content) != 2 * sat_chksum_len(type))
	  {
            fprintf(stderr, "Invalid checksum length: %d: for %s\n", (unsigned int)XML_GetCurrentLineNumber(*pd->parser), pd->tmpattr);
            exit(1);
	  }
        repodata_set_checksum(pd->data, pd->rdhandle, pd->state == STATE_CHECKSUM ? REPOSITORY_REPOMD_CHECKSUM : REPOSITORY_REPOMD_OPENCHECKSUM, type, pd->content);
        break;
      }

    case STATE_TIMESTAMP:
      {
        /**
         * we want to look for the newest timestamp
         * of all resources to save it as the time
         * the metadata was generated
         */
        int timestamp = atoi(pd->content);
	if (timestamp)
          repodata_set_num(pd->data, pd->rdhandle, REPOSITORY_REPOMD_TIMESTAMP, timestamp);
        if (timestamp > pd->timestamp)
          pd->timestamp = timestamp;
        break;
      }
    case STATE_EXPIRE:
      {
        int expire = atoi(pd->content);
	if (expire > 0)
	  repodata_set_num(pd->data, SOLVID_META, REPOSITORY_EXPIRE, expire);
        break;
      }
      /* repomd.xml content and suseinfo.xml keywords are equivalent */
    case STATE_CONTENT:
    case STATE_KEYWORD:
      if (pd->content)
	repodata_add_poolstr_array(pd->data, SOLVID_META, REPOSITORY_KEYWORDS, pd->content);
      break;
    case STATE_REVISION:
      if (pd->content)
	repodata_add_poolstr_array(pd->data, SOLVID_META, REPOSITORY_REVISION, pd->content);
      break;
    case STATE_DISTRO:
      /* distro tag is used in repomd.xml to say the product this repo is
         made for */
      if (pd->content)
        repodata_set_str(pd->data, pd->rphandle, REPOSITORY_PRODUCT_LABEL, pd->content);
      repodata_add_flexarray(pd->data, SOLVID_META, REPOSITORY_DISTROS, pd->rphandle);
      break;
    case STATE_UPDATES:
      /* distro tag is used in suseinfo.xml to say the repo updates a product
         however it s not yet a tag standarized for repomd.xml */
      if (pd->content)
        repodata_set_str(pd->data, pd->ruhandle, REPOSITORY_PRODUCT_LABEL, pd->content);
      repodata_add_flexarray(pd->data, SOLVID_META, REPOSITORY_UPDATES, pd->ruhandle);
      break;
    case STATE_REPO:
      if (pd->content)
	repodata_add_poolstr_array(pd->data, SOLVID_META, REPOSITORY_REPOID, pd->content);
      break;
    case STATE_SUSEINFO: break;
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
repo_add_repomdxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  Repodata *data;
  char buf[BUFF_SIZE];
  int i, l;
  struct stateswitch *sw;

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.timestamp = 0;
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
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &pd);
  pd.parser = &parser;
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  pool_debug(pool, SAT_FATAL, "repo_repomdxml: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
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

/* EOF */
