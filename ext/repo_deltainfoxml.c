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
#include "repo_deltainfoxml.h"

/*
 * <deltainfo>
 *   <newpackage name="libtool" epoch="0" version="1.5.24" release="6.fc9" arch="i386">
 *     <delta oldepoch="0" oldversion="1.5.24" oldrelease="3.fc8">
 *       <filename>DRPMS/libtool-1.5.24-3.fc8_1.5.24-6.fc9.i386.drpm</filename>
 *       <sequence>libtool-1.5.24-3.fc8-d3571f98b048b1a870e40241bb46c67ab4</sequence>
 *       <size>22452</size>
 *       <checksum type="sha">8f05394695dee9399c204614e21e5f6848990ab7</checksum>
 *     </delta>
 *     <delta oldepoch="0" oldversion="1.5.22" oldrelease="11.fc7">
 *       <filename>DRPMS/libtool-1.5.22-11.fc7_1.5.24-6.fc9.i386.drpm</filename>
 *        <sequence>libtool-1.5.22-11.fc7-e82691677eee1e83b4812572c5c9ce8eb</sequence>
 *        <size>110362</size>
 *        <checksum type="sha">326658fee45c0baec1e70231046dbaf560f941ce</checksum>
 *      </delta>
 *    </newpackage>
 *  </deltainfo>
 */

enum state {
  STATE_START,
  STATE_NEWPACKAGE,     /* 1 */
  STATE_DELTA,          /* 2 */
  STATE_FILENAME,       /* 3 */
  STATE_SEQUENCE,       /* 4 */
  STATE_SIZE,           /* 5 */
  STATE_CHECKSUM,       /* 6 */
  STATE_LOCATION,       /* 7 */
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  /* compatibility with old yum-presto */
  { STATE_START,       "prestodelta",     STATE_START, 0 },
  { STATE_START,       "deltainfo",       STATE_START, 0 },
  { STATE_START,       "newpackage",      STATE_NEWPACKAGE,  0 },
  { STATE_NEWPACKAGE,  "delta",           STATE_DELTA,       0 },
  /* compatibility with yum-presto */
  { STATE_DELTA,       "filename",        STATE_FILENAME,    1 },
  { STATE_DELTA,       "location",        STATE_LOCATION,    0 },
  { STATE_DELTA,       "sequence",        STATE_SEQUENCE,    1 },
  { STATE_DELTA,       "size",            STATE_SIZE,        1 },
  { STATE_DELTA,       "checksum",        STATE_CHECKSUM,    1 },
  { NUMSTATES }
};

/* Cumulated info about the current deltarpm or patchrpm */
struct deltarpm {
  char *location;
  char *locbase;
  unsigned int buildtime;
  unsigned long long downloadsize;
  char *filechecksum;
  int filechecksumtype;
  /* Baseversion.  deltarpm only has one. */
  Id *bevr;
  unsigned nbevr;
  Id seqname;
  Id seqevr;
  char *seqnum;
};

struct parsedata {
  int ret;
  Pool *pool;
  Repo *repo;
  Repodata *data;

  struct deltarpm delta;
  Id newpkgevr;
  Id newpkgname;
  Id newpkgarch;

  Id *handles;
  int nhandles;

  struct solv_xmlparser xmlp;
};


/*
 * create evr (as Id) from 'epoch', 'version' and 'release' attributes
 */

static Id
makeevr_atts(Pool *pool, struct parsedata *pd, const char **atts)
{
  const char *e, *v, *r, *v2;
  char *c, *space;
  int l;

  e = v = r = 0;
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, "oldepoch"))
        e = atts[1];
      else if (!strcmp(*atts, "epoch"))
	e = atts[1];
      else if (!strcmp(*atts, "version"))
	v = atts[1];
      else if (!strcmp(*atts, "oldversion"))
	v = atts[1];
      else if (!strcmp(*atts, "release"))
	r = atts[1];
      else if (!strcmp(*atts, "oldrelease"))
	r = atts[1];
    }
  if (e && (!*e || !strcmp(e, "0")))
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
  c = space = solv_xmlparser_contentspace(&pd->xmlp, l);
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
  if (!*space)
    return 0;
#if 0
  fprintf(stderr, "evr: %s\n", space);
#endif
  return pool_str2id(pool, space, 1);
}

static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  const char *str;

  switch(state)
    {
    case STATE_NEWPACKAGE:
      if ((str = solv_xmlparser_find_attr("name", atts)) != 0)
	pd->newpkgname = pool_str2id(pool, str, 1);
      pd->newpkgevr = makeevr_atts(pool, pd, atts);
      if ((str = solv_xmlparser_find_attr("arch", atts)) != 0)
	pd->newpkgarch = pool_str2id(pool, str, 1);
      break;

    case STATE_DELTA:
      memset(&pd->delta, 0, sizeof(pd->delta));
      pd->delta.bevr = solv_extend(pd->delta.bevr, pd->delta.nbevr, 1, sizeof(Id), 7);
      pd->delta.bevr[pd->delta.nbevr++] = makeevr_atts(pool, pd, atts);
      break;

    case STATE_FILENAME:
      if ((str = solv_xmlparser_find_attr("xml:base", atts)))
        pd->delta.locbase = solv_strdup(str);
      break;

    case STATE_LOCATION:
      pd->delta.location = solv_strdup(solv_xmlparser_find_attr("href", atts));
      if ((str = solv_xmlparser_find_attr("xml:base", atts)))
        pd->delta.locbase = solv_strdup(str);
      break;

    case STATE_CHECKSUM:
      pd->delta.filechecksum = 0;
      pd->delta.filechecksumtype = REPOKEY_TYPE_SHA1;
      if ((str = solv_xmlparser_find_attr("type", atts)) != 0)
	{
	  pd->delta.filechecksumtype = solv_chksum_str2type(str);
	  if (!pd->delta.filechecksumtype)
	    pool_debug(pool, SOLV_ERROR, "unknown checksum type: '%s'\n", str);
	}
      break;

    default:
      break;
    }
}


static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  const char *str;

  switch (state)
    {
    case STATE_DELTA:
      {
	/* read all data for a deltarpm. commit into attributes */
	Id handle;
	struct deltarpm *d = &pd->delta;

	handle = repodata_new_handle(pd->data);
	/* we commit all handles later on in one go so that the
         * repodata code doesn't need to realloc every time */
	pd->handles = solv_extend(pd->handles, pd->nhandles, 1, sizeof(Id), 63);
        pd->handles[pd->nhandles++] = handle;
	repodata_set_id(pd->data, handle, DELTA_PACKAGE_NAME, pd->newpkgname);
	repodata_set_id(pd->data, handle, DELTA_PACKAGE_EVR, pd->newpkgevr);
	repodata_set_id(pd->data, handle, DELTA_PACKAGE_ARCH, pd->newpkgarch);
	if (d->location)
	  {
	    repodata_set_deltalocation(pd->data, handle, 0, 0, d->location);
	    if (d->locbase)
	      repodata_set_poolstr(pd->data, handle, DELTA_LOCATION_BASE, d->locbase);
	  }
	if (d->downloadsize)
	  repodata_set_num(pd->data, handle, DELTA_DOWNLOADSIZE, d->downloadsize);
	if (d->filechecksum)
	  repodata_set_checksum(pd->data, handle, DELTA_CHECKSUM, d->filechecksumtype, d->filechecksum);
	if (d->seqnum)
	  {
	    repodata_set_id(pd->data, handle, DELTA_BASE_EVR, d->bevr[0]);
	    repodata_set_id(pd->data, handle, DELTA_SEQ_NAME, d->seqname);
	    repodata_set_id(pd->data, handle, DELTA_SEQ_EVR, d->seqevr);
	    /* should store as binary blob! */
	    repodata_set_str(pd->data, handle, DELTA_SEQ_NUM, d->seqnum);
	  }
      }
      pd->delta.filechecksum = solv_free(pd->delta.filechecksum);
      pd->delta.bevr = solv_free(pd->delta.bevr);
      pd->delta.nbevr = 0;
      pd->delta.seqnum = solv_free(pd->delta.seqnum);
      pd->delta.location = solv_free(pd->delta.location);
      pd->delta.locbase = solv_free(pd->delta.locbase);
      break;
    case STATE_FILENAME:
      pd->delta.location = solv_strdup(content);
      break;
    case STATE_CHECKSUM:
      pd->delta.filechecksum = solv_strdup(content);
      break;
    case STATE_SIZE:
      pd->delta.downloadsize = strtoull(content, 0, 10);
      break;
    case STATE_SEQUENCE:
      if ((str = content) != 0)
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
		      pd->delta.seqevr = pool_strn2id(pool, s2 + 1, s1 - s2 - 1, 1);
		      pd->delta.seqname = pool_strn2id(pool, str, s2 - str, 1);
		      str = s1 + 1;
		    }
		}
	    }
	  pd->delta.seqnum = solv_strdup(str);
      }
    default:
      break;
    }
}

void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pd->ret = pool_error(pd->pool, -1, "repo_deltainfoxml: %s at line %u:%u", errstr, line, column);
}

int
repo_add_deltainfoxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  struct parsedata pd;
  int i;

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);

  /* now commit all handles */
  if (!pd.ret)
    for (i = 0; i < pd.nhandles; i++)
      repodata_add_flexarray(pd.data, SOLVID_META, REPOSITORY_DELTAINFO, pd.handles[i]);
  solv_free(pd.handles);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return pd.ret;
}

/* EOF */
