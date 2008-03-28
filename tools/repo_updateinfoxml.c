
/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <expat.h>

#include "pool.h"
#include "repo.h"
#include "repo_patchxml.h"
#include "repo_rpmmd.h"

//#define TESTMM

/*
<updates>
  <update from="rel-eng@fedoraproject.org" status="stable" type="security" version="1.4">
    <id>FEDORA-2007-4594</id>
    <title>imlib-1.9.15-6.fc8</title>
    <release>Fedora 8</release>
    <issued date="2007-12-28 16:42:30"/>
    <references>
      <reference href="https://bugzilla.redhat.com/show_bug.cgi?id=426091" id="426091" title="CVE-2007-3568 imlib: infinite loop DoS using crafted BMP image" type="bugzilla"/>
    </references>
    <description>This update includes a fix for a denial-of-service issue (CVE-2007-3568) whereby an attacker who could get an imlib-using user to view a  specially-crafted BMP image could cause the user's CPU to go into an infinite loop.</description>
    <pkglist>
      <collection short="F8">
        <name>Fedora 8</name>
        <package arch="ppc64" name="imlib-debuginfo" release="6.fc8" src="http://download.fedoraproject.org/pub/fedora/linux/updates/8/ppc64/imlib-debuginfo-1.9.15-6.fc8.ppc64.rpm" version="1.9.15">
          <filename>imlib-debuginfo-1.9.15-6.fc8.ppc64.rpm</filename>
        </package>
*/

enum state {
  STATE_START,
  STATE_UPDATES,
  STATE_UPDATE,
  STATE_ID,
  STATE_TITLE,
  STATE_RELEASE,
  STATE_ISSUED,
  STATE_REFERENCES,
  STATE_REFERENCE,
  STATE_DESCRIPTION,
  STATE_PKGLIST,
  STATE_COLLECTION,
  STATE_NAME,
  STATE_PACKAGE,
  STATE_FILENAME,
  NUMSTATES
};

struct stateswitch {
  enum state from;
  char *ename;
  enum state to;
  int docontent;
};

static struct stateswitch stateswitches[] = {
  { STATE_START,       "updates",         STATE_UPDATES, 0 },
  { STATE_START,       "update",          STATE_UPDATE, 0 },
  { STATE_UPDATES,     "update",          STATE_UPDATE, 0},
  { STATE_UPDATE,      "id",              STATE_ID, 1},
  { STATE_UPDATE,      "title",           STATE_TITLE, 1},
  { STATE_UPDATE,      "release",         STATE_RELEASE, 1},
  { STATE_UPDATE,      "issued",          STATE_ISSUED, 1},
  { STATE_UPDATE,      "references",      STATE_REFERENCES, 0},
  { STATE_UPDATE,      "description",     STATE_DESCRIPTION, 0},
  { STATE_REFERENCES,  "reference",       STATE_REFERENCE, 0},
  { STATE_UPDATE,      "pkglist",         STATE_PKGLIST, 0},
  { STATE_PKGLIST,     "collection",      STATE_COLLECTION, 0},
  { STATE_COLLECTION,  "name",            STATE_NAME, 1},
  { STATE_COLLECTION,  "package",         STATE_PACKAGE, 0},
  { STATE_COLLECTION,  "filename",        STATE_FILENAME, 1},
  { NUMSTATES}
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
  char *kind;
  unsigned int timestamp;
  
  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
  char *tempstr;
  int ltemp;
  int atemp;
};

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

static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  /*Pool *pool = pd->pool;*/
  /*Solvable *s = pd->solvable;*/
  struct stateswitch *sw;
  /*const char *str; */

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
#if 0
      fprintf(stderr, "into unknown: %s\n", name);
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
     default:
      break;
    }
}

static void XMLCALL
endElement(void *userData, const char *name)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;

  if (pd->depth != pd->statedepth)
    {
      pd->depth--;
      // printf("back from unknown %d %d %d\n", pd->state, pd->depth, pd->statedepth);
      return;
    }

  pd->depth--;
  pd->statedepth--;
  switch (pd->state)
    {
        case STATE_ID:
            s->name = str2id(pool, pd->content, 1);
            break;
        case STATE_TITLE:
            repodata_set_str(pd->data, pd->datanum, SOLVABLE_SUMMARY, pd->content);
            break;
        case STATE_RELEASE:
        case STATE_ISSUED:
            s->name = str2id(pool, pd->content, 1);
        case STATE_REFERENCE:
        case STATE_DESCRIPTION:
            repodata_set_str(pd->data, pd->datanum, SOLVABLE_DESCRIPTION, pd->content);
            break;
        case STATE_NAME:
      

        default:
            break;
    }



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
}
