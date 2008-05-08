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
  STATE_DELTAINFO,      /* 1 */
  STATE_NEWPACKAGE,     /* 2 */
  STATE_DELTA,          /* 3 */
  STATE_FILENAME,       /* 4 */
  STATE_SEQUENCE,       /* 5 */
  STATE_SIZE,           /* 6 */
  STATE_CHECKSUM,       /* 7 */
  STATE_LOCATION,       /* 8 */
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
  { STATE_START,       "deltainfo",       STATE_DELTAINFO,   0 },
  /* compatibility with old yum-presto */
  { STATE_START,       "prestodelta",     STATE_DELTAINFO,   0 },
  /* allow starting from newpackage directly */
  { STATE_START,       "newpackage",      STATE_NEWPACKAGE,  0 },
  { STATE_DELTAINFO,   "newpackage",      STATE_NEWPACKAGE,  0 },
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
  Id locdir;
  Id locname;
  Id locevr;
  Id locsuffix;
  unsigned buildtime;
  unsigned downloadsize, archivesize;
  char *filechecksum;
  
  /* Baseversion.  deltarpm only has one. */
  Id *bevr;
  unsigned nbevr;
  Id seqname;
  Id seqevr;
  char *seqnum;
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
  
  struct stateswitch *swtab[NUMSTATES];
  enum state sbtab[NUMSTATES];
  char *tempstr;
  int ltemp;
  int atemp;
  struct deltarpm delta;
  Id newpkgevr;
  Id newpkgname;
};

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

static void parse_delta_location( struct parsedata *pd, 
                                  const char* str )
{
    Pool *pool = pd->pool;
    if (str)
    {
        /* Separate the filename into its different parts.
           rpm/x86_64/alsa-1.0.14-31_31.2.x86_64.delta.rpm
           --> dir = rpm/x86_64
           name = alsa
           evr = 1.0.14-31_31.2
           suffix = x86_64.delta.rpm.  */
        char *real_str = strdup(str);
        char *s = real_str;
        char *s1, *s2;
        s1 = strrchr (s, '/');
        if (s1)
        {
            pd->delta.locdir = strn2id(pool, s, s1 - s, 1);
            s = s1 + 1;
        }
        /* Guess suffix.  */
        s1 = strrchr (s, '.');
        if (s1)
        {
            for (s2 = s1 - 1; s2 > s; s2--)
                if (*s2 == '.')
                    break;
            if (!strcmp (s2, ".delta.rpm") || !strcmp (s2, ".patch.rpm"))
            {
                s1 = s2;
                /* We accept one more item as suffix.  */
                for (s2 = s1 - 1; s2 > s; s2--)
		    if (*s2 == '.')
                        break;
                s1 = s2;
                  }
            if (*s1 == '.')
                *s1++ = 0;
            pd->delta.locsuffix = str2id(pool, s1, 1); 
        }
        /* Last '-'.  */
        s1 = strrchr (s, '-');
        if (s1)
        {
                  /* Second to last '-'.  */
            for (s2 = s1 - 1; s2 > s; s2--)
                if (*s2 == '-')
                    break;
        }
        else
            s2 = 0;
        if (s2 > s && *s2 == '-')
        {
            *s2++ = 0;
            pd->delta.locevr = str2id(pool, s2, 1);
        }
        pd->delta.locname = str2id(pool, s, 1);
        free(real_str);
    }
}
                                 
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
      case STATE_NEWPACKAGE:
          if ( (str = find_attr("name", atts)) )
          {
              pd->newpkgname = str2id(pool, str, 1);
          }
          break;
          
      case STATE_DELTA:
          memset(&pd->delta, 0, sizeof (pd->delta));
          *pd->tempstr = 0;
          pd->ltemp = 0;
          pd->delta.nbevr++;
          pd->delta.bevr = sat_realloc (pd->delta.bevr, pd->delta.nbevr * sizeof(Id));
          pd->delta.bevr[pd->delta.nbevr - 1] = makeevr_atts(pool, pd, atts);
          break;
      case STATE_FILENAME:
          break;
      case STATE_LOCATION:
          parse_delta_location( pd, find_attr("href", atts));
          break;
    case STATE_SIZE:
      break;
    case STATE_SEQUENCE:
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
  const char *str;

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
      case STATE_NEWPACKAGE:
          break;
      case STATE_DELTA:
      {
#if DUMPOUT
          int i;
          struct deltarpm *d = &pd->delta;
          fprintf (stderr, "found deltarpm for %s:\n", id2str(pool, pd->newpkgname));
          fprintf (stderr, "   loc: %s %s %s %s\n", id2str(pool, d->locdir),
                   id2str(pool, d->locname), id2str(pool, d->locevr),
                   id2str(pool, d->locsuffix));
          fprintf (stderr, "  size: %d down\n", d->downloadsize);
          fprintf (stderr, "  chek: %s\n", d->filechecksum);
          if (d->seqnum)
	  {
              fprintf (stderr, "  base: %s\n",
                       id2str(pool, d->bevr[0]));
              fprintf (stderr, "            seq: %s\n",
                       id2str(pool, d->seqname));
              fprintf (stderr, "                 %s\n",
                       id2str(pool, d->seqevr));
              fprintf (stderr, "                 %s\n",
                       d->seqnum);

              fprintf(stderr, "OK\n");
              
              if (d->seqevr != d->bevr[0])
                  fprintf (stderr, "XXXXX evr\n");
              /* Name of package ("atom:xxxx") should match the sequence info
                 name.  */
              if (strcmp(id2str(pool, d->seqname), id2str(pool, pd->newpkgname) + 5))
                  fprintf (stderr, "XXXXX name\n");
	  }
          else
	  {
              fprintf (stderr, "  base:");
              for (i = 0; i < d->nbevr; i++)
                  fprintf (stderr, " %s", id2str(pool, d->bevr[i]));
              fprintf (stderr, "\n");
	  }
#endif
      }
      free(pd->delta.filechecksum);
      free(pd->delta.bevr);
      free(pd->delta.seqnum);
      break;
      case STATE_FILENAME:
          parse_delta_location(pd, pd->content);
          break;
      case STATE_CHECKSUM:
      pd->delta.filechecksum = strdup(pd->content);
      break;
      case STATE_SIZE:
          pd->delta.downloadsize = atoi(pd->content);
          break;
      case STATE_SEQUENCE:
      if ((str = pd->content))
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
		      pd->delta.seqevr = strn2id(pool, s2 + 1, s1 - s2 - 1, 1);
		      pd->delta.seqname = strn2id(pool, str, s2 - str, 1);
		      str = s1 + 1;
                  }
              }
          }
	  pd->delta.seqnum = strdup(str);
      }
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
repo_add_deltainfoxml(Repo *repo, FILE *fp, int flags)
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
