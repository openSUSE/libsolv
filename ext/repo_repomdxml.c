/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "repo.h"
#include "chksum.h"
#include "solv_xmlparser.h"
#include "repo_repomdxml.h"

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
  STATE_SIZE,
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
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
  { STATE_DATA,        "open-checksum",   STATE_OPENCHECKSUM, 1 },
  { STATE_DATA,        "size",            STATE_SIZE, 1 },
  { NUMSTATES }
};


struct parsedata {
  int ret;
  Pool *pool;
  Repo *repo;
  Repodata *data;

  struct solv_xmlparser xmlp;

  int timestamp;
  /* handles for collection
     structures */
  /* repo updates */
  Id ruhandle;
  /* repo products */
  Id rphandle;
  /* repo data handle */
  Id rdhandle;

  Id chksumtype;
};


static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;

  switch(state)
    {
    case STATE_REPOMD:
      {
        const char *updstr;

        /* this should be OBSOLETE soon */
        updstr = solv_xmlparser_find_attr("updates", atts);
        if (updstr)
          {
            char *value = solv_strdup(updstr);
            char *fvalue = value; /* save the first */
            while (value)
	      {
		char *p = strchr(value, ',');
		if (*p)
		  *p++ = 0;
		if (*value)
		  repodata_add_poolstr_array(pd->data, SOLVID_META, REPOSITORY_UPDATES, value);
		value = p;
	      }
	    solv_free(fvalue);
          }
        break;
      }
    case STATE_DISTRO:
      {
        /* this is extra metadata about the product this repository
           was designed for */
        const char *cpeid = solv_xmlparser_find_attr("cpeid", atts);
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
        const char *cpeid = solv_xmlparser_find_attr("cpeid", atts);
        pd->ruhandle = repodata_new_handle(pd->data);
        /* set the cpeid for the product
           the label is set in the content of the tag */
        if (cpeid)
          repodata_set_poolstr(pd->data, pd->ruhandle, REPOSITORY_PRODUCT_CPEID, cpeid);
        break;
      }
    case STATE_DATA:
      {
        const char *type= solv_xmlparser_find_attr("type", atts);
        pd->rdhandle = repodata_new_handle(pd->data);
	if (type)
          repodata_set_poolstr(pd->data, pd->rdhandle, REPOSITORY_REPOMD_TYPE, type);
        break;
      }
    case STATE_LOCATION:
      {
        const char *href = solv_xmlparser_find_attr("href", atts);
	if (href)
          repodata_set_str(pd->data, pd->rdhandle, REPOSITORY_REPOMD_LOCATION, href);
        break;
      }
    case STATE_CHECKSUM:
    case STATE_OPENCHECKSUM:
      {
        const char *type= solv_xmlparser_find_attr("type", atts);
        pd->chksumtype = type && *type ? solv_chksum_str2type(type) : 0;
	if (!pd->chksumtype)
          pd->ret = pool_error(pd->pool, -1, "line %d: unknown checksum type: %s", solv_xmlparser_lineno(xmlp), type ? type : "NULL");
        break;
      }
    default:
      break;
    }
  return;
}

static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  switch (state)
    {
    case STATE_REPOMD:
      if (pd->timestamp > 0)
        repodata_set_num(pd->data, SOLVID_META, REPOSITORY_TIMESTAMP, pd->timestamp);
      break;
    case STATE_DATA:
      if (pd->rdhandle)
        repodata_add_flexarray(pd->data, SOLVID_META, REPOSITORY_REPOMD, pd->rdhandle);
      pd->rdhandle = 0;
      break;

    case STATE_CHECKSUM:
    case STATE_OPENCHECKSUM:
      if (!pd->chksumtype)
	break;
      if (strlen(content) != 2 * solv_chksum_len(pd->chksumtype))
        pd->ret = pool_error(pd->pool, -1, "line %d: invalid checksum length for %s", solv_xmlparser_lineno(xmlp), solv_chksum_type2str(pd->chksumtype));
      else
        repodata_set_checksum(pd->data, pd->rdhandle, state == STATE_CHECKSUM ? REPOSITORY_REPOMD_CHECKSUM : REPOSITORY_REPOMD_OPENCHECKSUM, pd->chksumtype, content);
      break;

    case STATE_TIMESTAMP:
      {
        /**
         * we want to look for the newest timestamp
         * of all resources to save it as the time
         * the metadata was generated
         */
        int timestamp = atoi(content);
	if (timestamp)
          repodata_set_num(pd->data, pd->rdhandle, REPOSITORY_REPOMD_TIMESTAMP, timestamp);
        if (timestamp > pd->timestamp)
          pd->timestamp = timestamp;
        break;
      }
    case STATE_EXPIRE:
      {
        int expire = atoi(content);
	if (expire > 0)
	  repodata_set_num(pd->data, SOLVID_META, REPOSITORY_EXPIRE, expire);
        break;
      }
      /* repomd.xml content and suseinfo.xml keywords are equivalent */
    case STATE_CONTENT:
    case STATE_KEYWORD:
      if (*content)
	repodata_add_poolstr_array(pd->data, SOLVID_META, REPOSITORY_KEYWORDS, content);
      break;
    case STATE_REVISION:
      if (*content)
	repodata_set_str(pd->data, SOLVID_META, REPOSITORY_REVISION, content);
      break;
    case STATE_DISTRO:
      /* distro tag is used in repomd.xml to say the product this repo is
         made for */
      if (*content)
        repodata_set_str(pd->data, pd->rphandle, REPOSITORY_PRODUCT_LABEL, content);
      repodata_add_flexarray(pd->data, SOLVID_META, REPOSITORY_DISTROS, pd->rphandle);
      break;
    case STATE_UPDATES:
      /* updates tag is used in suseinfo.xml to say the repo updates a product
         however it s not yet a tag standarized for repomd.xml */
      if (*content)
        repodata_set_str(pd->data, pd->ruhandle, REPOSITORY_PRODUCT_LABEL, content);
      repodata_add_flexarray(pd->data, SOLVID_META, REPOSITORY_UPDATES, pd->ruhandle);
      break;
    case STATE_REPO:
      if (*content)
	repodata_add_poolstr_array(pd->data, SOLVID_META, REPOSITORY_REPOID, content);
      break;
    case STATE_SIZE:
      if (*content)
	repodata_set_num(pd->data, pd->rdhandle, REPOSITORY_REPOMD_SIZE, strtoull(content, 0, 10));
      break;
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pd->ret = pool_error(pd->pool, -1, "repo_repomdxml: %s at line %u:%u", errstr, line, column);
}

int
repo_add_repomdxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
  Repodata *data;

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.timestamp = 0;
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);

  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);

  return pd.ret;
}

/* EOF */
