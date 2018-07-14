/*
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "chksum.h"
#include "solv_xmlparser.h"
#include "repo_mdk.h"

static Offset
parse_deps(Solvable *s, char *bp, Id marker)
{
  Pool *pool = s->repo->pool;
  Offset deps = 0;
  char *nbp, *ebp;
  for (; bp; bp = nbp)
    {
      int ispre = 0;
      Id id, evr = 0;
      int flags = 0;

      nbp = strchr(bp, '@');
      if (!nbp)
	ebp = bp + strlen(bp);
      else
	{
	  ebp = nbp;
	  *nbp++ = 0;
	}
      if (ebp[-1] == ']')
	{
	  char *sbp = ebp - 1;
	  while (sbp >= bp && *sbp != '[')
	    sbp--;
	  if (sbp >= bp && sbp[1] != '*')
	    {
	      char *fbp;
	      for (fbp = sbp + 1;; fbp++)
		{
		  if (*fbp == '>')
		    flags |= REL_GT;
		  else if (*fbp == '=')
		    flags |= REL_EQ;
		  else if (*fbp == '<')
		    flags |= REL_LT;
		  else
		    break;
		}
	      if (*fbp == ' ')
		fbp++;
	      evr = pool_strn2id(pool, fbp, ebp - 1 - fbp, 1);
	      ebp = sbp;
	    }
	}
      if (ebp[-1] == ']' && ebp >= bp + 3 && !strncmp(ebp - 3, "[*]", 3))
	{
	  ispre = 1;
	  ebp -= 3;
	}
      id = pool_strn2id(pool, bp, ebp - bp, 1);
      if (evr)
	id = pool_rel2id(pool, id, evr, flags, 1);
      deps = repo_addid_dep(s->repo, deps, id, ispre ? marker : 0);
      bp = nbp;
    }
  return deps;
}

int
repo_add_mdk(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data;
  Solvable *s;
  char *buf;
  int bufa, bufl;

  data = repo_add_repodata(repo, flags);
  bufa = 4096;
  buf = solv_malloc(bufa);
  bufl = 0;
  s = 0;
  while (fgets(buf + bufl, bufa - bufl, fp) > 0)
    {
      bufl += strlen(buf + bufl);
      if (!bufl)
	continue;
      if (buf[bufl - 1] != '\n')
	{
	  if (bufa - bufl < 256)
	    {
	      bufa += 4096;
	      buf = solv_realloc(buf, bufa);
	    }
	  continue;
	}
      buf[bufl - 1] = 0;
      bufl = 0;
      if (buf[0] != '@')
	{
	  pool_debug(pool, SOLV_ERROR, "bad line <%s>\n", buf);
	  continue;
	}
      if (!s)
	s = pool_id2solvable(pool, repo_add_solvable(repo));
      if (!strncmp(buf + 1, "filesize@", 9))
	repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, strtoull(buf + 10, 0, 10));
      else if (!strncmp(buf + 1, "summary@", 8))
	repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, buf + 9);
      else if (!strncmp(buf + 1, "provides@", 9))
	s->provides = parse_deps(s, buf + 10, 0);
      else if (!strncmp(buf + 1, "requires@", 9))
	s->requires = parse_deps(s, buf + 10, SOLVABLE_PREREQMARKER);
      else if (!strncmp(buf + 1, "recommends@", 11))
	s->recommends = parse_deps(s, buf + 10, 0);
      else if (!strncmp(buf + 1, "suggests@", 9))
	s->suggests = parse_deps(s, buf + 10, 0);
      else if (!strncmp(buf + 1, "obsoletes@", 10))
	s->obsoletes = parse_deps(s, buf + 11, 0);
      else if (!strncmp(buf + 1, "conflicts@", 10))
	s->conflicts = parse_deps(s, buf + 11, 0);
      else if (!strncmp(buf + 1, "info@", 5))
	{
	  char *nvra = buf + 6;
	  char *epochstr;
	  char *arch;
	  char *version;
	  char *filename;
	  char *disttag = 0;
	  char *distepoch = 0;
	  if ((epochstr = strchr(nvra, '@')) != 0)
	    {
	      char *sizestr;
	      *epochstr++ = 0;
	      if ((sizestr = strchr(epochstr, '@')) != 0)
		{
		  char *groupstr;
		  *sizestr++ = 0;
		  if ((groupstr = strchr(sizestr, '@')) != 0)
		    {
		      *groupstr++ = 0;
		      if ((disttag = strchr(groupstr, '@')) != 0)
			{
			  *disttag++ = 0;
			  if ((distepoch = strchr(disttag, '@')) != 0)
			    {
			      char *n;
			      *distepoch++ = 0;
			      if ((n = strchr(distepoch, '@')) != 0)
				*n = 0;
			    }
			}
		      if (*groupstr)
			repodata_set_poolstr(data, s - pool->solvables, SOLVABLE_GROUP, groupstr);
		    }
		  if (*sizestr)
		    repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLSIZE, strtoull(sizestr, 0, 10));
		}
	    }
          filename = pool_tmpjoin(pool, nvra, ".rpm", 0);
	  arch = strrchr(nvra, '.');
	  if (arch)
	    {
	      *arch++ = 0;
	      s->arch = pool_str2id(pool, arch, 1);
	    }
	  if (disttag && *disttag)
	    {
	      /* strip disttag from release */
	      char *n = strrchr(nvra, '-');
	      if (n && !strncmp(n + 1, disttag, strlen(disttag)))
		*n = 0;
	    }
	  if (distepoch && *distepoch)
	    {
	      /* add distepoch */
	      int le = strlen(distepoch);
	      int ln = strlen(nvra);
	      nvra[ln++] = ':';
	      memmove(nvra + ln, distepoch, le);	/* may overlap */
	      nvra[le + ln] = 0;
	    }
	  version = strrchr(nvra, '-');
	  if (version)
	    {
	      char *release = version;
	      *release = 0;
	      version = strrchr(nvra, '-');
	      *release = '-';
	      if (!version)
		version = release;
	      *version++ = 0;
	    }
	  else
	    version = "";
	  s->name = pool_str2id(pool, nvra, 1);
	  if (epochstr && *epochstr && strcmp(epochstr, "0") != 0)
	    {
	      char *evr = pool_tmpjoin(pool, epochstr, ":", version);
	      s->evr = pool_str2id(pool, evr, 1);
	    }
	  else
	    s->evr = pool_str2id(pool, version, 1);
	  repodata_set_location(data, s - pool->solvables, 0, 0, filename);
	  if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	    s->provides = repo_addid_dep(s->repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
          s = 0;
	}
      else
	{
	  char *tagend = strchr(buf + 1, '@');
	  if (tagend)
	    *tagend = 0;
	  pool_debug(pool, SOLV_ERROR, "unknown tag <%s>\n", buf + 1);
	  continue;
	}
    }
  if (s)
    {
      pool_debug(pool, SOLV_ERROR, "unclosed package at EOF\n");
      repo_free_solvable(s->repo, s - pool->solvables, 1);
    }
  solv_free(buf);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}

enum state {
  STATE_START,
  STATE_MEDIA_INFO,
  STATE_INFO,
  STATE_FILES,
  NUMSTATES
};

static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START, "media_info", STATE_MEDIA_INFO, 0 },
  { STATE_MEDIA_INFO, "info", STATE_INFO, 1 },
  { STATE_MEDIA_INFO, "files", STATE_FILES, 1 },
  { NUMSTATES }
};

struct parsedata {
  Pool *pool;
  Repo *repo;
  Repodata *data;
  Solvable *solvable;
  Hashtable joinhash;
  Hashval joinhashmask;
  struct solv_xmlparser xmlp;
};

static Hashtable
joinhash_init(Repo *repo, Hashval *hmp)
{
  Hashval hm = mkmask(repo->nsolvables);
  Hashtable ht = solv_calloc(hm + 1, sizeof(*ht));
  Hashval h, hh;
  Solvable *s;
  int i;

  FOR_REPO_SOLVABLES(repo, i, s)
    {
      hh = HASHCHAIN_START;
      h = s->name & hm;
      while (ht[h])
        h = HASHCHAIN_NEXT(h, hh, hm);
      ht[h] = i;
    }
  *hmp = hm;
  return ht;
}

static Solvable *
joinhash_lookup(Repo *repo, Hashtable ht, Hashval hm, const char *fn, const char *distepoch)
{
  Hashval h, hh;
  const char *p, *vrstart, *vrend;
  Id name, arch;

  if (!fn || !*fn)
    return 0;
  if (distepoch && !*distepoch)
    distepoch = 0;
  p = fn + strlen(fn);
  while (--p > fn)
    if (*p == '.')
      break;
  if (p == fn)
    return 0;
  arch = pool_str2id(repo->pool, p + 1, 0);
  if (!arch)
    return 0;
  if (distepoch)
    {
      while (--p > fn)
        if (*p == '-')
          break;
      if (p == fn)
	return 0;
    }
  vrend = p;
  while (--p > fn)
    if (*p == '-')
      break;
  if (p == fn)
    return 0;
  while (--p > fn)
    if (*p == '-')
      break;
  if (p == fn)
    return 0;
  vrstart = p + 1;
  name = pool_strn2id(repo->pool, fn, p - fn, 0);
  if (!name)
    return 0;
  hh = HASHCHAIN_START;
  h = name & hm;
  while (ht[h])
    {
      Solvable *s = repo->pool->solvables + ht[h];
      if (s->name == name && s->arch == arch)
	{
	  /* too bad we don't know the epoch... */
	  const char *evr = pool_id2str(repo->pool, s->evr);
	  for (p = evr; *p >= '0' && *p <= '9'; p++)
	    ;
	  if (p > evr && *p == ':')
	    evr = p + 1;
	  if (distepoch)
	    {
              if (!strncmp(evr, vrstart, vrend - vrstart) && evr[vrend - vrstart] == ':' && !strcmp(distepoch, evr + (vrend - vrstart + 1)))
	        return s;
	    }
          else if (!strncmp(evr, vrstart, vrend - vrstart) && evr[vrend - vrstart] == 0)
	    return s;
	}
      h = HASHCHAIN_NEXT(h, hh, hm);
    }
  return 0;
}

static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;

  switch (state)
    {
    case STATE_INFO:
      {
	const char *fn = solv_xmlparser_find_attr("fn", atts);
	const char *distepoch = solv_xmlparser_find_attr("distepoch", atts);
	const char *str;
	pd->solvable = joinhash_lookup(pd->repo, pd->joinhash, pd->joinhashmask, fn, distepoch);
	if (!pd->solvable)
	  break;
	str = solv_xmlparser_find_attr("url", atts);
	if (str && *str)
	  repodata_set_str(pd->data, pd->solvable - pool->solvables, SOLVABLE_URL, str);
	str = solv_xmlparser_find_attr("license", atts);
	if (str && *str)
	  repodata_set_poolstr(pd->data, pd->solvable - pool->solvables, SOLVABLE_LICENSE, str);
	str = solv_xmlparser_find_attr("sourcerpm", atts);
	if (str && *str)
	  repodata_set_sourcepkg(pd->data, pd->solvable - pool->solvables, str);
        break;
      }
    case STATE_FILES:
      {
	const char *fn = solv_xmlparser_find_attr("fn", atts);
	const char *distepoch = solv_xmlparser_find_attr("distepoch", atts);
	pd->solvable = joinhash_lookup(pd->repo, pd->joinhash, pd->joinhashmask, fn, distepoch);
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
  switch (state)
    {
    case STATE_INFO:
      if (s && *content)
        repodata_set_str(pd->data, s - pd->pool->solvables, SOLVABLE_DESCRIPTION, content);
      break;
    case STATE_FILES:
      if (s && *content)
	{
	  char *np, *p, *sl;
	  for (p = content; p && *p; p = np)
	    {
	      Id id;
	      np = strchr(p, '\n');
	      if (np)
		*np++ = 0;
	      if (!*p)
		continue;
	      sl = strrchr(p, '/');
	      if (sl)
		{
		  *sl++ = 0;
		  id = repodata_str2dir(pd->data, p, 1);
		}
	      else
		{
		  sl = p;
		  id = 0;
		}
	      if (!id)
		id = repodata_str2dir(pd->data, "/", 1);
	      repodata_add_dirstr(pd->data, s - pd->pool->solvables, SOLVABLE_FILELIST, id, sl);
	    }
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
  pool_debug(pd->pool, SOLV_ERROR, "%s at line %u:%u\n", errstr, line, column);
}


int
repo_add_mdk_info(Repo *repo, FILE *fp, int flags)
{
  Repodata *data;
  struct parsedata pd;

  if (!(flags & REPO_EXTEND_SOLVABLES))
    {
      pool_debug(repo->pool, SOLV_ERROR, "repo_add_mdk_info: can only extend existing solvables\n");
      return -1;
    }

  data = repo_add_repodata(repo, flags);

  memset(&pd, 0, sizeof(pd));
  pd.repo = repo;
  pd.pool = repo->pool;
  pd.data = data;
  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  pd.joinhash = joinhash_init(repo, &pd.joinhashmask);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);
  solv_free(pd.joinhash);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}
