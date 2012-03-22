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
      if (bufl && buf[bufl - 1] != '\n')
	{
	  if (bufa - bufl < 256)
	    {
	      bufa += 4096;
	      buf = solv_realloc(buf, bufa);
	    }
	  continue;
	}
      buf[--bufl] = 0;
      bufl = 0;
      if (buf[0] != '@')
	{
	  pool_debug(pool, SOLV_ERROR, "bad line <%s>\n", buf);
	  continue;
	}
      if (!s)
	s = pool_id2solvable(pool, repo_add_solvable(repo));
      if (!strncmp(buf + 1, "filesize@", 9))
	{
	  unsigned long filesize = strtoul(buf + 10, 0, 10);
	  repodata_set_num(data, s - pool->solvables, SOLVABLE_DOWNLOADSIZE, (unsigned int)((filesize + 1023) / 1024));
	}
      else if (!strncmp(buf + 1, "summary@", 8))
	repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, buf + 9);
      else if (!strncmp(buf + 1, "provides@", 9))
	s->provides = parse_deps(s, buf + 10, 0);
      else if (!strncmp(buf + 1, "requires@", 9))
	s->requires = parse_deps(s, buf + 10, SOLVABLE_PREREQMARKER);
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
	  if ((epochstr = strchr(nvra, '@')) != 0)
	    {
	      char *sizestr;
	      *epochstr++ = 0;
	      if ((sizestr = strchr(epochstr, '@')) != 0)
		{
		  char *groupstr;
		  unsigned long size;
		  *sizestr++ = 0;
		  if ((groupstr = strchr(sizestr, '@')) != 0)
		    {
		      char *n;
		      *groupstr++ = 0;
		      if ((n = strchr(groupstr, '@')) != 0)
			*n = 0;
		      if (*groupstr)
			repodata_set_poolstr(data, s - pool->solvables, SOLVABLE_GROUP, groupstr);
		    }
		  size = strtoul(sizestr, 0, 10);
		  repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLSIZE, (unsigned int)((size + 1023) / 1024));
		}
	    }
          filename = pool_tmpjoin(pool, nvra, ".rpm", 0);
	  arch = strrchr(nvra, '.');
	  if (arch)
	    {
	      *arch++ = 0;
	      s->arch = pool_str2id(pool, arch, 1);
	    }
	  /* argh, do we have a distepoch or not, check self-provides */
	  if (s->provides)
	    {
	      Id id, lastid, *idp = s->repo->idarraydata + s->provides;
	      lastid = 0;
	      for (idp = s->repo->idarraydata + s->provides; (id = *idp) != 0; idp++)
		{
		  const char *evr, *name;
		  int namel;
		  Reldep *rd;
		  if (!ISRELDEP(id))
		    continue;
		  rd = GETRELDEP(pool, id);
		  if (rd->flags != REL_EQ)
		    continue;
		  name = pool_id2str(pool, rd->name);
		  namel = strlen(name);
		  if (strncmp(name, nvra, namel) != 0 || nvra[namel] != '-')
		    continue;
		  evr = pool_id2str(pool, rd->evr);
		  evr = strrchr(evr, '-');
		  if (evr && strchr(evr, ':') != 0)
		    lastid = id;
		}
	      if (lastid)
		{
		  /* self provides found, and it contains a distepoch */
		  /* replace with self-provides distepoch to get rid of the disttag */
		  char *nvradistepoch = strrchr(nvra, '-');
		  if (nvradistepoch)
		    {
		      Reldep *rd = GETRELDEP(pool, lastid);
		      const char *evr = pool_id2str(pool, rd->evr);
		      evr = strrchr(evr, '-');
		      if (evr && (evr = strchr(evr, ':')) != 0)
			{
			  if (strlen(evr) < strlen(nvradistepoch))
			    strcpy(nvradistepoch, evr);
			}
		    }
		}
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
      repo_free_solvable_block(s->repo, s - pool->solvables, 1, 1);
    }
  solv_free(buf);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}
