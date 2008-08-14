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
#include "repo_diskusagexml.h"

//#define DUMPOUT 0

/*
 * <diskusage>
 *   <duinfo name="3ddiag" ver="0.742" rel="45" arch="i586">
 *     <dirs>
 *       <dir name="/" size="56" count="11"/>
 *       <dir name="usr/" size="56" count="11"/>
 *       <dir name="usr/bin/" size="38" count="10"/>
 *       <dir name="usr/share/" size="18" count="1"/>
 *       <dir name="usr/share/doc/" size="18" count="1"/>
 *     </dirs>
 *   </duinfo>
 *   <duinfo name="915resolution" ver="0.5.3" rel="74" arch="i586">
 *     <dirs>
 *       <dir name="/" size="27" count="7"/>
 * ...
 *     </dirs>
 *   </duinfo>
 * </diskusage>
 */

enum state {
  STATE_START,
  STATE_DISKUSAGE, /* 1 */
  STATE_DUINFO,    /* 2 */
  STATE_DIRS,      /* 3 */
  STATE_DIR,       /* 4 */
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
  { STATE_START,       "diskusage",      STATE_DISKUSAGE,    0 },
  { STATE_DISKUSAGE,   "duinfo",         STATE_DUINFO,       0 },
  { STATE_DUINFO,      "dirs",           STATE_DIRS,         0 },
  { STATE_DIRS,        "dir",            STATE_DIR,          0 },
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
  char *tempstr;
  int ltemp;
  int atemp;
  Solvable *s;
  Id handle;

  Id (*dirs)[3]; // dirid, size, nfiles
  int ndirs;
};

/*
 * id3_cmp
 * compare
 *
 */

static int
id3_cmp (const void *v1, const void *v2)
{
  Id *i1 = (Id*)v1;
  Id *i2 = (Id*)v2;
  return i1[0] - i2[0];
}


/*
 * commit_diskusage
 *
 */

static void
commit_diskusage (struct parsedata *pd, unsigned handle)
{
  unsigned i;
  Dirpool *dp = &pd->data->dirpool;
  /* Now sort in dirid order.  This ensures that parents come before
     their children.  */
  if (pd->ndirs > 1)
    qsort(pd->dirs, pd->ndirs, sizeof (pd->dirs[0]), id3_cmp);
  /* Substract leaf numbers from all parents to make the numbers
     non-cumulative.  This must be done post-order (i.e. all leafs
     adjusted before parents).  We ensure this by starting at the end of
     the array moving to the start, hence seeing leafs before parents.  */
  for (i = pd->ndirs; i--;)
    {
      unsigned p = dirpool_parent(dp, pd->dirs[i][0]);
      unsigned j = i;
      for (; p; p = dirpool_parent(dp, p))
        {
          for (; j--;)
	    if (pd->dirs[j][0] == p)
	      break;
	  if (j < pd->ndirs)
	    {
	      if (pd->dirs[j][1] < pd->dirs[i][1])
	        pd->dirs[j][1] = 0;
	      else
	        pd->dirs[j][1] -= pd->dirs[i][1];
	      if (pd->dirs[j][2] < pd->dirs[i][2])
	        pd->dirs[j][2] = 0;
	      else
	        pd->dirs[j][2] -= pd->dirs[i][2];
	    }
	  else
	    /* Haven't found this parent in the list, look further if
	       we maybe find the parents parent.  */
	    j = i;
	}
    }
#if 0
  char sbuf[1024];
  char *buf = sbuf;
  unsigned slen = sizeof (sbuf);
  for (i = 0; i < pd->ndirs; i++)
    {
      dir2str (attr, pd->dirs[i][0], &buf, &slen);
      fprintf (stderr, "have dir %d %d %d %s\n", pd->dirs[i][0], pd->dirs[i][1], pd->dirs[i][2], buf);
    }
  if (buf != sbuf)
    free (buf);
#endif
  for (i = 0; i < pd->ndirs; i++)
    if (pd->dirs[i][1] || pd->dirs[i][2])
      {
	repodata_add_dirnumnum(pd->data, handle, SOLVABLE_DISKUSAGE, pd->dirs[i][0], pd->dirs[i][1], pd->dirs[i][2]);
      }
  pd->ndirs = 0;
}


/*
 * find attribute
 */

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


/*
 * XML callback: startElement
 */

static void XMLCALL
startElement(void *userData, const char *name, const char **atts)
{
  struct parsedata *pd = userData;
  Pool *pool = pd->pool;
  struct stateswitch *sw;
  const char *str;

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
      case STATE_DUINFO:
          pd->s = pool_id2solvable(pd->pool, repo_add_solvable(pd->repo));
          repodata_extend(pd->data, pd->s - pd->pool->solvables);
	  pd->handle = repodata_get_handle(pd->data, pd->s - pd->pool->solvables - pd->repo->start);
          if ( (str = find_attr("name", atts)) )
            {
              pd->s->name = str2id(pool, str, 1);
            }
          pd->s->evr = makeevr_atts(pool, pd, atts);
          if ( (str = find_attr("arch", atts)) )
            {
              pd->s->arch = str2id(pool, str, 1);
            }
          break;

      case STATE_DIR:
        {
	  long filesz = 0, filenum = 0;
          unsigned dirid;
          if ( (str = find_attr("name", atts)) )
            {
	      dirid = repodata_str2dir(pd->data, str, 1);
	    }
	  else
	    {	      
	      fprintf( stderr, "<dir .../> tag without 'name' attribute, atts = %p, *atts = %p\n", atts, *atts);
	      break;
	    }
          if ( (str = find_attr("size", atts)) )
            {
              filesz = strtol (str, 0, 0);
            }
          if ( (str = find_attr("count", atts)) )
            {
              filenum = strtol (str, 0, 0);
            }
          pd->dirs = sat_extend(pd->dirs, pd->ndirs, 1, sizeof(pd->dirs[0]), 31);
          pd->dirs[pd->ndirs][0] = dirid;
          pd->dirs[pd->ndirs][1] = filesz;
          pd->dirs[pd->ndirs][2] = filenum;
          pd->ndirs++;
	}
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
      case STATE_DUINFO:
        if (pd->ndirs)
          commit_diskusage (pd, pd->handle);
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
repo_add_diskusagexml(Repo *repo, FILE *fp, int flags)
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
  pd.datanum = 0;
  XML_Parser parser = XML_ParserCreate(NULL);
  XML_SetUserData(parser, &pd);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  for (;;)
    {
      l = fread(buf, 1, sizeof(buf), fp);
      if (XML_Parse(parser, buf, l, l == 0) == XML_STATUS_ERROR)
	{
	  fprintf(stderr, "repo_diskusagexml: %s at line %u:%u\n", XML_ErrorString(XML_GetErrorCode(parser)), (unsigned int)XML_GetCurrentLineNumber(parser), (unsigned int)XML_GetCurrentColumnNumber(parser));
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
