/*
 * repo_comps.c
 *
 * Parses RedHat comps format
 *
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "solv_xmlparser.h"
#define DISABLE_SPLIT
#include "tools_util.h"
#include "repo_comps.h"

/*
 * TODO:
 *
 * what's the difference between group/category?
 * handle "default" and "langonly".
 *
 * maybe handle REL_COND in solver recommends handling?
 */

enum state {
  STATE_START,
  STATE_COMPS,
  STATE_GROUP,
  STATE_ID,
  STATE_NAME,
  STATE_DESCRIPTION,
  STATE_DISPLAY_ORDER,
  STATE_DEFAULT,
  STATE_LANGONLY,
  STATE_LANG_ONLY,
  STATE_USERVISIBLE,
  STATE_PACKAGELIST,
  STATE_PACKAGEREQ,
  STATE_CATEGORY,
  STATE_CID,
  STATE_CNAME,
  STATE_CDESCRIPTION,
  STATE_CDISPLAY_ORDER,
  STATE_GROUPLIST,
  STATE_GROUPID,
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START,       "comps",         STATE_COMPS,         0 },
  { STATE_COMPS,       "group",         STATE_GROUP,         0 },
  { STATE_COMPS,       "category",      STATE_CATEGORY,      0 },
  { STATE_GROUP,       "id",            STATE_ID,            1 },
  { STATE_GROUP,       "name",          STATE_NAME,          1 },
  { STATE_GROUP,       "description",   STATE_DESCRIPTION,   1 },
  { STATE_GROUP,       "uservisible",   STATE_USERVISIBLE,   1 },
  { STATE_GROUP,       "display_order", STATE_DISPLAY_ORDER, 1 },
  { STATE_GROUP,       "default",       STATE_DEFAULT,       1 },
  { STATE_GROUP,       "langonly",      STATE_LANGONLY,      1 },
  { STATE_GROUP,       "lang_only",     STATE_LANG_ONLY,     1 },
  { STATE_GROUP,       "packagelist",   STATE_PACKAGELIST,   0 },
  { STATE_PACKAGELIST, "packagereq",    STATE_PACKAGEREQ,    1 },
  { STATE_CATEGORY,    "id",            STATE_ID,            1 },
  { STATE_CATEGORY,    "name",          STATE_NAME,          1 },
  { STATE_CATEGORY,    "description",   STATE_DESCRIPTION,   1 },
  { STATE_CATEGORY ,   "grouplist",     STATE_GROUPLIST,     0 },
  { STATE_CATEGORY ,   "display_order", STATE_DISPLAY_ORDER, 1 },
  { STATE_GROUPLIST,   "groupid",       STATE_GROUPID,       1 },
  { NUMSTATES }
};

struct parsedata {
  Pool *pool;
  Repo *repo;
  Repodata *data;
  const char *filename;
  const char *basename;

  struct solv_xmlparser xmlp;
  struct joindata jd;

  const char *tmplang;
  Id reqtype;
  Id condreq;

  Solvable *solvable;
  const char *kind;
  Id handle;
};



static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;

  switch(state)
    {
    case STATE_GROUP:
    case STATE_CATEGORY:
      s = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
      pd->handle = s - pool->solvables;
      pd->kind = state == STATE_GROUP ? "group" : "category";
      break;

    case STATE_NAME:
    case STATE_CNAME:
    case STATE_DESCRIPTION:
    case STATE_CDESCRIPTION:
      pd->tmplang = join_dup(&pd->jd, solv_xmlparser_find_attr("xml:lang", atts));
      break;

    case STATE_PACKAGEREQ:
      {
	const char *type = solv_xmlparser_find_attr("type", atts);
	pd->condreq = 0;
	pd->reqtype = SOLVABLE_RECOMMENDS;
	if (type && !strcmp(type, "conditional"))
	  {
	    const char *requires = solv_xmlparser_find_attr("requires", atts);
	    if (requires && *requires)
	      pd->condreq = pool_str2id(pool, requires, 1);
	  }
	else if (type && !strcmp(type, "mandatory"))
	  pd->reqtype = SOLVABLE_REQUIRES;
	else if (type && !strcmp(type, "optional"))
	  pd->reqtype = SOLVABLE_SUGGESTS;
	break;
      }

    default:
      break;
    }
}


static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  Solvable *s = pd->solvable;
  Id id;

  switch (state)
    {
    case STATE_GROUP:
    case STATE_CATEGORY:
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (!s->evr)
	s->evr = ID_EMPTY;
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pd->pool, s->name, s->evr, REL_EQ, 1), 0);
      pd->solvable = 0;
      break;

    case STATE_ID:
      s->name = pool_str2id(pd->pool, join2(&pd->jd, pd->kind, ":", content), 1);
      break;

    case STATE_NAME:
      repodata_set_str(pd->data, pd->handle, pool_id2langid(pd->pool, SOLVABLE_SUMMARY, pd->tmplang, 1), content);
      break;

    case STATE_DESCRIPTION:
      repodata_set_str(pd->data, pd->handle, pool_id2langid(pd->pool, SOLVABLE_DESCRIPTION, pd->tmplang, 1), content);
      break;

    case STATE_PACKAGEREQ:
      id = pool_str2id(pd->pool, content, 1);
      if (pd->condreq)
	id = pool_rel2id(pd->pool, id, pd->condreq, REL_COND, 1);
      repo_add_idarray(pd->repo, pd->handle, pd->reqtype, id);
      break;

    case STATE_GROUPID:
      id = pool_str2id(pd->pool, join2(&pd->jd, "group", ":", content), 1);
      s->requires = repo_addid_dep(pd->repo, s->requires, id, 0);
      break;

    case STATE_USERVISIBLE:
      repodata_set_void(pd->data, pd->handle, SOLVABLE_ISVISIBLE);
      break;

    case STATE_DISPLAY_ORDER:
      repodata_set_str(pd->data, pd->handle, SOLVABLE_ORDER, content);
      break;

    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pool_debug(pd->pool, SOLV_ERROR, "repo_comps: %s at line %u:%u\n", errstr, line, column);
}


int
repo_add_comps(Repo *repo, FILE *fp, int flags)
{
  Repodata *data;
  struct parsedata pd;

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.repo = repo;
  pd.pool = repo->pool;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);
  join_freemem(&pd.jd);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}

/* EOF */
