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
#include "tools_util.h"

/*
 * <updates>
 *   <update from="rel-eng@fedoraproject.org" status="stable" type="security" version="1.4">
 *     <id>FEDORA-2007-4594</id>
 *     <title>imlib-1.9.15-6.fc8</title>
 *     <release>Fedora 8</release>
 *     <issued date="2007-12-28 16:42:30"/>
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
  STATE_UPDATES,      /* 1 */
  STATE_UPDATE,       /* 2 */
  STATE_ID,           /* 3 */
  STATE_TITLE,        /* 4 */
  STATE_RELEASE,      /* 5 */
  STATE_ISSUED,       /* 6 */
  STATE_MESSAGE,      /* 7 */
  STATE_REFERENCES,   /* 8 */
  STATE_REFERENCE,    /* 9 */
  STATE_DESCRIPTION,  /* 10 */
  STATE_PKGLIST,     /* 11 */
  STATE_COLLECTION,  /* 12 */
  STATE_NAME,        /* 13 */
  STATE_PACKAGE,     /* 14 */
  STATE_FILENAME,    /* 15 */
  STATE_REBOOT,      /* 16 */
  STATE_RESTART,     /* 17 */
  STATE_RELOGIN,     /* 18 */
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
  { STATE_START,       "updates",         STATE_UPDATES,     0 },
  { STATE_START,       "update",          STATE_UPDATE,      0 },
  { STATE_UPDATES,     "update",          STATE_UPDATE,      0 },
  { STATE_UPDATE,      "id",              STATE_ID,          1 },
  { STATE_UPDATE,      "title",           STATE_TITLE,       1 },
  { STATE_UPDATE,      "release",         STATE_RELEASE,     1 },
  { STATE_UPDATE,      "issued",          STATE_ISSUED,      1 },
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
  unsigned int datanum;
  Solvable *solvable;
  unsigned int timestamp;
  

  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
  char *tempstr;
  int ltemp;
  int atemp;
};

/*
 * if we have seen a <filename>...
 * inside of <package>...
 * 
 *
 * If not, we must insert an empty filename to UPDATE_COLLECTION_FILENAME
 * at </package> in order to keep all UPDATE_COLLECTION_* arrays in sync
 */

static int package_filename_seen = 0;
static int package_flags = 0; /* same for reboot/restart flags, to be written at </package> */


/*
 * find attribute
 */

/*
static const char *
find_attr(const char *txt, const char **atts)
{
  for (; *atts; atts += 2)
    {
      if (!strcmp(*atts, txt))
        return atts[1];
    }
  return 0;
}
*/


/*
 * create evr (as Id) from 'epoch', 'version' and 'release' attributes
 */

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
      else if (!strcmp(*atts, "version"))
	v = atts[1];
      else if (!strcmp(*atts, "release"))
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



static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  Solvable *solvable = pd->solvable;
  struct stateswitch *sw;
  /*const char *str; */

#if 0
      fprintf(stderr, "start: [%d]%s\n", pd->state, name);
#endif
  if (pd->depth != pd->statedepth)
    {
      pd->depth++;
      return;
    }

  pd->depth++;
  for (sw = pd->swtab[pd->state]; sw->from == pd->state; sw++)  /* find name in statetable */
    if (!strcmp(sw->ename, name))
      break;
  

  if (sw->from != pd->state)
    {
#if 1
      fprintf(stderr, "into unknown: [%d]%s (from: %d)\n", sw->to, name, sw->from);
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
      case STATE_START:
      break;
      case STATE_UPDATES:
      break;
      /*
       * <update from="rel-eng@fedoraproject.org"
       *         status="stable"
       *         type="bugfix" (enhancement, security)
       *         version="1.4">
       */
      case STATE_UPDATE:
      {
	const char *from = 0, *status = 0, *type = 0, *version = 0;
	for (; *atts; atts += 2)
	{
	  if (!strcmp(*atts, "from"))
	    from = atts[1];
	  else if (!strcmp(*atts, "status"))
	    status = atts[1];
	  else if (!strcmp(*atts, "type"))
	    type = atts[1];
	  else if (!strcmp(*atts, "version"))
	    version = atts[1];
	}
	

	solvable = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
	pd->datanum = (pd->solvable - pool->solvables) - pd->repo->start;
	repodata_extend(pd->data, pd->solvable - pool->solvables);      
	repodata_extend(pd->data, pd->solvable - pool->solvables);
	pd->datanum = repodata_get_handle(pd->data, pd->datanum);
	

	solvable->vendor = str2id(pool, from, 1);
	solvable->evr = str2id(pool, version, 1);
	solvable->arch = ARCH_NOARCH;
	repodata_set_str(pd->data, pd->datanum, SOLVABLE_PATCHCATEGORY, type);
      }
      break;
      /* <id>FEDORA-2007-4594</id> */
      case STATE_ID:
      break;
      /* <title>imlib-1.9.15-6.fc8</title> */
      case STATE_TITLE:
      break;
      /* <release>Fedora 8</release> */
      case STATE_RELEASE:
      break;
      /*  <issued date="2008-03-21 21:36:55"/>
      */
      case STATE_ISSUED:
      {
	const char *date = 0;
	for (; *atts; atts += 2)
	{
	  if (!strcmp(*atts, "date"))
	    date = atts[1];
	}
	repodata_set_str(pd->data, pd->datanum, SOLVABLE_BUILDTIME, date);
      }
      break;
      case STATE_REFERENCES:
      break;
      /*  <reference href="https://bugzilla.redhat.com/show_bug.cgi?id=330471"
       *             id="330471"
       *             title="LDAP schema file missing for dhcpd"
       *             type="bugzilla"/>
       */
      case STATE_REFERENCE:
      {
        const char *href = 0, *id = 0, *title = 0, *type = 0;
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
#if DO_ARRAY
        repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_REFERENCE_HREF, href);
        repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_REFERENCE_ID, id);
        repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_REFERENCE_TITLE, title);
        repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_REFERENCE_TYPE, type);
#endif
      }
      break;
      /* <description>This update ...</description> */
      case STATE_DESCRIPTION:
      break;
      /* <message type="confirm">This update ...</message> */
      case STATE_MESSAGE:
      break;
      case STATE_PKGLIST:
      break;
      /* <collection short="F8"> */
      case STATE_COLLECTION:
      break;
      /* <name>Fedora 8</name> */ 
      case STATE_NAME:
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
	const char *arch = 0, *name = 0, *src = 0;
	Id evr = makeevr_atts(pool, pd, atts); /* parse "epoch", "version", "release" */
	Id n, a, na;
	Id rel_id;
	

	/* reset package_* markers, to be evaluated at </package> */
	package_filename_seen = 0;
	package_flags = 0;
	

	for (; *atts; atts += 2)
	{
	  if (!strcmp(*atts, "arch"))
	    arch = atts[1];
	  else if (!strcmp(*atts, "name"))
	    name = atts[1];
	  else if (!strcmp(*atts, "src"))
	    src = atts[1];
	}
	/* generated Ids for name and arch */
	n = str2id(pool, name, 1);
	if (arch)
	  a = str2id(pool, arch, 1);
	else
	  a = ARCH_NOARCH;
	/*  now combine both to a single Id */
	na = rel2id(pool, n, a, REL_ARCH, 1);
	

	rel_id = rel2id(pool, na, evr, REL_LT, 1);

	solvable->conflicts = repo_addid_dep(pd->repo, solvable->conflicts, rel_id, 0);
#if DO_ARRAY
	repodata_add_idarray(pd->data, pd->datanum, UPDATE_COLLECTION_NAME, n);
	repodata_add_idarray(pd->data, pd->datanum, UPDATE_COLLECTION_EVR, evr);
	repodata_add_idarray(pd->data, pd->datanum, UPDATE_COLLECTION_ARCH, a);
#else
	/* _FILENAME and _FLAGS are written at </package> */
        if (1) {
	  const char *evrstr = id2str(pool, evr);
	  int buflen = strlen(name) + 1 + strlen(evrstr) + 1 + strlen(arch?arch:"") + 1;
	  char *buf;
	  if (!arch) arch = "";
	  buf = (char *)malloc(buflen);
	  if (!buf) exit(1);
	  sprintf(buf, "%s %s %s", name, evrstr, arch);
	  repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_COLLECTION, buf);
	  free(buf);
	}
#endif
      }
      break;
      /* <filename>libntlm-0.4.2-1.fc8.x86_64.rpm</filename> */ 
      /* <filename>libntlm-0.4.2-1.fc8.x86_64.rpm</filename> */
      case STATE_FILENAME:
      break;
      /* <reboot_suggested>True</reboot_suggested> */
      case STATE_REBOOT:
      break;
      /* <restart_suggested>True</restart_suggested> */
      case STATE_RESTART:
      break;
      /* <relogin_suggested>True</relogin_suggested> */
      case STATE_RELOGIN:
      break;
      case NUMSTATES+1:
        split(NULL, NULL, 0); /* just to keep gcc happy about tools_util.h: static ... split() {...}  Urgs!*/
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
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  Repo *repo = pd->repo;

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
      case STATE_START:
      break;
      case STATE_UPDATES:
      break;
      case STATE_UPDATE:
      s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      break;
      case STATE_ID:
      {
        if (pd->content) {
	  s->name = str2id(pool, join2("patch", ":", pd->content), 1);
	}
      }
      break;
      /* <title>imlib-1.9.15-6.fc8</title> */
      case STATE_TITLE:
      {
	while (pd->lcontent > 0
	       && *(pd->content + pd->lcontent - 1) == '\n')
	{
	  --pd->lcontent;
	  *(pd->content + pd->lcontent) = 0;
	}
	repodata_set_str(pd->data, pd->datanum, SOLVABLE_SUMMARY, pd->content);
      }
      break;
      /*
       * <release>Fedora 8</release>
       */
      case STATE_RELEASE:
      break;
      case STATE_ISSUED:
      break;
      case STATE_REFERENCES:
      break;
      case STATE_REFERENCE:
      break;
      /*
       * <description>This update ...</description>
       */
      case STATE_DESCRIPTION:
      {
	repodata_set_str(pd->data, pd->datanum, SOLVABLE_DESCRIPTION, pd->content);
      }
      break;   
      break;
      /*
       * <message>Warning! ...</message>
       */
      case STATE_MESSAGE:
      {
	repodata_set_str(pd->data, pd->datanum, UPDATE_MESSAGE, pd->content);
      }
      break;
      case STATE_PKGLIST:
      break;
      case STATE_COLLECTION:
      break;
      case STATE_NAME:
      break;
      case STATE_PACKAGE:
      {
#if DO_ARRAY
	/* write _FILENAME and _FLAGS at </package>
	 * to ensure all UPDATE_COLLECTION_* arrays are filled in parallel
	 */
	if (!package_filename_seen)
	{
	  repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_COLLECTION_FILENAME, "");
	}
	repodata_add_idarray(pd->data, pd->datanum, UPDATE_COLLECTION_FLAGS, package_flags+1);
#endif
      }
      break;
      /* <filename>libntlm-0.4.2-1.fc8.x86_64.rpm</filename> */ 
      /* <filename>libntlm-0.4.2-1.fc8.x86_64.rpm</filename> */
      case STATE_FILENAME:
      {
#if DO_ARRAY
	repodata_add_poolstr_array(pd->data, pd->datanum, UPDATE_COLLECTION_FILENAME, pd->content);
        package_filename_seen = 1;
#endif
      }
      break;
      /* <reboot_suggested>True</reboot_suggested> */
      case STATE_REBOOT:
      {
	if (pd->content
	    && (pd->content[0] == 'T'
		|| pd->content[0] == 't'|| pd->content[0] == '1'))
	{
	  /* FIXME: this is per-package, the global flag should be computed at runtime */
	  repodata_set_void(pd->data, pd->datanum, UPDATE_REBOOT);
	  package_flags = 1;
	}
      }
      break;
      /* <restart_suggested>True</restart_suggested> */
      case STATE_RESTART:
      {
	if (pd->content
	    && (pd->content[0] == 'T'
		|| pd->content[0] == 't' || pd->content[0] == '1'))
	{
	  /* FIXME: this is per-package, the global flag should be computed at runtime */
	  repodata_set_void(pd->data, pd->datanum, UPDATE_RESTART);
	  package_flags = 2;
	}
      }
      break;
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
repo_add_updateinfoxml(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  struct parsedata pd;
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
  pd.tempstr = malloc(256);
  pd.atemp = 256;
  pd.ltemp = 0;
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &pd);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  fprintf(stderr, "repo_updateinfoxml: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
	  exit(1);
	}
      if (l == 0)
	break;
    }
  XML_ParserFree(parser);

  if (pd.data)
    repodata_internalize(pd.data);

  free(pd.content);
  join_freemem();
}

/* EOF */
