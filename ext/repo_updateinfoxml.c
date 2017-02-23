/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE
#define _XOPEN_SOURCE /* glibc2 needs this */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pool.h"
#include "repo.h"
#include "solv_xmlparser.h"
#include "repo_updateinfoxml.h"
#define DISABLE_SPLIT
#include "tools_util.h"

/*
 * <updates>
 *   <update from="rel-eng@fedoraproject.org" status="stable" type="security" version="1.4">
 *     <id>FEDORA-2007-4594</id>
 *     <title>imlib-1.9.15-6.fc8</title>
 *     <severity>Important</severity>
 *     <release>Fedora 8</release>
 *     <rights>Copyright 2007 Company Inc</rights>
 *     <issued date="2007-12-28 16:42:30"/>
 *     <updated date="2008-03-14 12:00:00"/>
 *     <references>
 *       <reference href="https://bugzilla.redhat.com/show_bug.cgi?id=426091" id="426091" title="CVE-2007-3568 imlib: infinite loop DoS using crafted BMP image" type="bugzilla"/>
 *     </references>
 *     <description>This update includes a fix for a denial-of-service issue (CVE-2007-3568) whereby an attacker who could get an imlib-using user to view a  specially-crafted BMP image could cause the user's CPU to go into an infinite loop.</description>
 *     <pkglist>
 *       <collection short="F8">
 *         <name>Fedora 8</name>
 *         <package arch="ppc64" name="imlib-debuginfo" release="6.fc8" src="http://download.fedoraproject.org/pub/fedora/linux/updates/8/ppc64/imlib-debuginfo-1.9.15-6.fc8.ppc64.rpm" version="1.9.15">
 *           <filename>imlib-debuginfo-1.9.15-6.fc8.ppc64.rpm</filename>
 *           <reboot_suggested>True</reboot_suggested>
 *         </package>
 *       </collection>
 *     </pkglist>
 *   </update>
 * </updates>
*/

enum state {
  STATE_START,
  STATE_UPDATES,
  STATE_UPDATE,
  STATE_ID,
  STATE_TITLE,
  STATE_RELEASE,
  STATE_ISSUED,
  STATE_UPDATED,
  STATE_MESSAGE,
  STATE_REFERENCES,
  STATE_REFERENCE,
  STATE_DESCRIPTION,
  STATE_PKGLIST,
  STATE_COLLECTION,
  STATE_NAME,
  STATE_PACKAGE,
  STATE_FILENAME,
  STATE_REBOOT,
  STATE_RESTART,
  STATE_RELOGIN,
  STATE_RIGHTS,
  STATE_SEVERITY,
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START,       "updates",         STATE_UPDATES,     0 },
  { STATE_START,       "update",          STATE_UPDATE,      0 },
  { STATE_UPDATES,     "update",          STATE_UPDATE,      0 },
  { STATE_UPDATE,      "id",              STATE_ID,          1 },
  { STATE_UPDATE,      "title",           STATE_TITLE,       1 },
  { STATE_UPDATE,      "severity",        STATE_SEVERITY,    1 },
  { STATE_UPDATE,      "rights",          STATE_RIGHTS,      1 },
  { STATE_UPDATE,      "release",         STATE_RELEASE,     1 },
  { STATE_UPDATE,      "issued",          STATE_ISSUED,      0 },
  { STATE_UPDATE,      "updated",         STATE_UPDATED,     0 },
  { STATE_UPDATE,      "description",     STATE_DESCRIPTION, 1 },
  { STATE_UPDATE,      "message",         STATE_MESSAGE    , 1 },
  { STATE_UPDATE,      "references",      STATE_REFERENCES,  0 },
  { STATE_UPDATE,      "pkglist",         STATE_PKGLIST,     0 },
  { STATE_REFERENCES,  "reference",       STATE_REFERENCE,   0 },
  { STATE_PKGLIST,     "collection",      STATE_COLLECTION,  0 },
  { STATE_COLLECTION,  "name",            STATE_NAME,        1 },
  { STATE_COLLECTION,  "package",         STATE_PACKAGE,     0 },
  { STATE_PACKAGE,     "filename",        STATE_FILENAME,    1 },
  { STATE_PACKAGE,     "reboot_suggested",STATE_REBOOT,      1 },
  { STATE_PACKAGE,     "restart_suggested",STATE_RESTART,    1 },
  { STATE_PACKAGE,     "relogin_suggested",STATE_RELOGIN,    1 },
  { NUMSTATES }
};

struct parsedata {
  int ret;
  Pool *pool;
  Repo *repo;
  Repodata *data;
  Id handle;
  Solvable *solvable;
  time_t buildtime;
  Id collhandle;
  struct solv_xmlparser xmlp;
  struct joindata jd;
};

/*
 * Convert date strings ("1287746075" or "2010-10-22 13:14:35")
 * to timestamp.
 */
static time_t
datestr2timestamp(const char *date)
{
  const char *p;
  struct tm tm;

  if (!date || !*date)
    return 0;
  for (p = date; *p >= '0' && *p <= '9'; p++)
    ;
  if (!*p)
    return atoi(date);
  memset(&tm, 0, sizeof(tm));
  if (!strptime(date, "%F%T", &tm))
    return 0;
  return timegm(&tm);
}

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
      if (!strcmp(*atts, "epoch"))
	e = atts[1];
      else if (!strcmp(*atts, "version"))
	v = atts[1];
      else if (!strcmp(*atts, "release"))
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
  Solvable *solvable = pd->solvable;

  switch(state)
    {
      /*
       * <update from="rel-eng@fedoraproject.org"
       *         status="stable"
       *         type="bugfix" (enhancement, security)
       *         version="1.4">
       */
    case STATE_UPDATE:
      {
	const char *from = 0, *type = 0, *version = 0;
	for (; *atts; atts += 2)
	  {
	    if (!strcmp(*atts, "from"))
	      from = atts[1];
	    else if (!strcmp(*atts, "type"))
	      type = atts[1];
	    else if (!strcmp(*atts, "version"))
	      version = atts[1];
	  }
	solvable = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
	pd->handle = pd->solvable - pool->solvables;

	solvable->vendor = pool_str2id(pool, from, 1);
	solvable->evr = pool_str2id(pool, version, 1);
	solvable->arch = ARCH_NOARCH;
	if (type)
	  repodata_set_str(pd->data, pd->handle, SOLVABLE_PATCHCATEGORY, type);
        pd->buildtime = (time_t)0;
      }
      break;

    case STATE_ISSUED:
    case STATE_UPDATED:
      {
	const char *date = solv_xmlparser_find_attr("date", atts);
	if (date)
	  {
	    time_t t = datestr2timestamp(date);
	    if (t && t > pd->buildtime)
              pd->buildtime = t;
	  }
      }
      break;

    case STATE_REFERENCE:
      {
        const char *href = 0, *id = 0, *title = 0, *type = 0;
	Id refhandle;
	for (; *atts; atts += 2)
	  {
	    if (!strcmp(*atts, "href"))
	      href = atts[1];
	    else if (!strcmp(*atts, "id"))
	      id = atts[1];
	    else if (!strcmp(*atts, "title"))
	      title = atts[1];
	    else if (!strcmp(*atts, "type"))
	      type = atts[1];
	  }
	refhandle = repodata_new_handle(pd->data);
	if (href)
	  repodata_set_str(pd->data, refhandle, UPDATE_REFERENCE_HREF, href);
	if (id)
	  repodata_set_str(pd->data, refhandle, UPDATE_REFERENCE_ID, id);
	if (title)
	  repodata_set_str(pd->data, refhandle, UPDATE_REFERENCE_TITLE, title);
	if (type)
	  repodata_set_poolstr(pd->data, refhandle, UPDATE_REFERENCE_TYPE, type);
	repodata_add_flexarray(pd->data, pd->handle, UPDATE_REFERENCE, refhandle);
      }
      break;

      /*   <package arch="ppc64" name="imlib-debuginfo" release="6.fc8"
       *            src="http://download.fedoraproject.org/pub/fedora/linux/updates/8/ppc64/imlib-debuginfo-1.9.15-6.fc8.ppc64.rpm"
       *            version="1.9.15">
       *
       *
       * -> patch.conflicts: {name} < {version}.{release}
       */
    case STATE_PACKAGE:
      {
	const char *arch = 0, *name = 0;
	Id evr = makeevr_atts(pool, pd, atts); /* parse "epoch", "version", "release" */
	Id n, a = 0;
	Id rel_id;

	for (; *atts; atts += 2)
	  {
	    if (!strcmp(*atts, "arch"))
	      arch = atts[1];
	    else if (!strcmp(*atts, "name"))
	      name = atts[1];
	  }
	/* generated Id for name */
	n = pool_str2id(pool, name, 1);
	rel_id = n;
	if (arch)
	  {
	    /*  generate Id for arch and combine with name */
	    a = pool_str2id(pool, arch, 1);
	    rel_id = pool_rel2id(pool, n, a, REL_ARCH, 1);
	  }
	rel_id = pool_rel2id(pool, rel_id, evr, REL_LT, 1);
	solvable->conflicts = repo_addid_dep(pd->repo, solvable->conflicts, rel_id, 0);

        /* who needs the collection anyway? */
        pd->collhandle = repodata_new_handle(pd->data);
	repodata_set_id(pd->data, pd->collhandle, UPDATE_COLLECTION_NAME, n);
	repodata_set_id(pd->data, pd->collhandle, UPDATE_COLLECTION_EVR, evr);
	if (a)
	  repodata_set_id(pd->data, pd->collhandle, UPDATE_COLLECTION_ARCH, a);
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
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  Repo *repo = pd->repo;

  switch (state)
    {
    case STATE_UPDATE:
      s->provides = repo_addid_dep(repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      if (pd->buildtime)
	{
	  repodata_set_num(pd->data, pd->handle, SOLVABLE_BUILDTIME, pd->buildtime);
	  pd->buildtime = (time_t)0;
	}
      break;

    case STATE_ID:
      s->name = pool_str2id(pool, join2(&pd->jd, "patch", ":", content), 1);
      break;

      /* <title>imlib-1.9.15-6.fc8</title> */
    case STATE_TITLE:
      /* strip trailing newlines */
      while (pd->xmlp.lcontent > 0 && content[pd->xmlp.lcontent - 1] == '\n')
        content[--pd->xmlp.lcontent] = 0;
      repodata_set_str(pd->data, pd->handle, SOLVABLE_SUMMARY, content);
      break;

    case STATE_SEVERITY:
      repodata_set_poolstr(pd->data, pd->handle, UPDATE_SEVERITY, content);
      break;

    case STATE_RIGHTS:
      repodata_set_poolstr(pd->data, pd->handle, UPDATE_RIGHTS, content);
      break;

      /*
       * <description>This update ...</description>
       */
    case STATE_DESCRIPTION:
      repodata_set_str(pd->data, pd->handle, SOLVABLE_DESCRIPTION, content);
      break;

      /*
       * <message>Warning! ...</message>
       */
    case STATE_MESSAGE:
      repodata_set_str(pd->data, pd->handle, UPDATE_MESSAGE, content);
      break;

    case STATE_PACKAGE:
      repodata_add_flexarray(pd->data, pd->handle, UPDATE_COLLECTION, pd->collhandle);
      pd->collhandle = 0;
      break;

      /* <filename>libntlm-0.4.2-1.fc8.x86_64.rpm</filename> */
      /* <filename>libntlm-0.4.2-1.fc8.x86_64.rpm</filename> */
    case STATE_FILENAME:
      repodata_set_str(pd->data, pd->collhandle, UPDATE_COLLECTION_FILENAME, content);
      break;

      /* <reboot_suggested>True</reboot_suggested> */
    case STATE_REBOOT:
      if (content[0] == 'T' || content[0] == 't'|| content[0] == '1')
	{
	  /* FIXME: this is per-package, the global flag should be computed at runtime */
	  repodata_set_void(pd->data, pd->handle, UPDATE_REBOOT);
	  repodata_set_void(pd->data, pd->collhandle, UPDATE_REBOOT);
	}
      break;

      /* <restart_suggested>True</restart_suggested> */
    case STATE_RESTART:
      if (content[0] == 'T' || content[0] == 't'|| content[0] == '1')
	{
	  /* FIXME: this is per-package, the global flag should be computed at runtime */
	  repodata_set_void(pd->data, pd->handle, UPDATE_RESTART);
	  repodata_set_void(pd->data, pd->collhandle, UPDATE_RESTART);
	}
      break;

      /* <relogin_suggested>True</relogin_suggested> */
    case STATE_RELOGIN:
      if (content[0] == 'T' || content[0] == 't'|| content[0] == '1')
	{
	  /* FIXME: this is per-package, the global flag should be computed at runtime */
	  repodata_set_void(pd->data, pd->handle, UPDATE_RELOGIN);
	  repodata_set_void(pd->data, pd->collhandle, UPDATE_RELOGIN);
	}
      break;
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pd->ret = pool_error(pd->pool, -1, "repo_updateinfoxml: %s at line %u:%u", errstr, line, column);
}

int
repo_add_updateinfoxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  struct parsedata pd;

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.pool = pool;
  pd.repo = repo;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);
  join_freemem(&pd.jd);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return pd.ret;
}

