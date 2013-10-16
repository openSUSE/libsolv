/*
 * Copyright (c) 2013, SUSE Inc.
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
#include <errno.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "repo_autopattern.h"

static void
unescape(char *p)
{
  char *q = p;
  while (*p)
    {
      if (*p == '%' && p[1] && p[2])
	{
	  int d1 = p[1], d2 = p[2];
	  if (d1 >= '0' && d1 <= '9')
	    d1 -= '0';
	  else if (d1 >= 'a' && d1 <= 'f')
	    d1 -= 'a' - 10;
	  else if (d1 >= 'A' && d1 <= 'F')
	    d1 -= 'A' - 10;
	  else
	    d1 = -1;
	  if (d2 >= '0' && d2 <= '9')
	    d2 -= '0';
	  else if (d2 >= 'a' && d2 <= 'f')
	    d2 -= 'a' - 10;
	  else if (d2 >= 'A' && d2 <= 'F')
	    d2 -= 'A' - 10;
	  else
	    d2 = -1;
	  if (d1 != -1 && d2 != -1)
	    {
	      *q++ = d1 << 4 | d2;
	      p += 3;
	      continue;
	    }
	}
      *q++ = *p++;
    }
  *q = 0;
}

int
repo_add_autopattern(Repo *repo, int flags)
{
  Pool *pool = repo->pool;
  Repodata *data = 0;
  Solvable *s, *s2;
  Queue q, q2;
  Id p;
  Id pattern_id;
  Id autopattern_id = 0;
  int i, j;

  queue_init(&q);
  queue_init(&q2);

  pattern_id = pool_str2id(pool, "pattern()", 9);
  FOR_REPO_SOLVABLES(repo, p, s)
    {
      const char *n = pool_id2str(pool, s->name);
      if (!strncmp("pattern:", n, 8))
	queue_push(&q, p);
      else if (s->provides)
	{
	  Id prv, *prvp = repo->idarraydata + s->provides;
	  while ((prv = *prvp++) != 0)            /* go through all provides */
	    if (ISRELDEP(prv))
	      {
	        Reldep *rd = GETRELDEP(pool, prv);
		if (rd->name == pattern_id && rd->flags == REL_EQ)
		  {
		    queue_push2(&q2, p, rd->evr);
		    break;
		  }
	      }
	}
    }
  for (i = 0; i < q2.count; i += 2)
    {
      const char *pn = 0;
      char *newname;
      Id name, prv, *prvp;
      const char *str;
      unsigned long long num;

      s = pool->solvables + q2.elements[i];
      /* construct new name */
      newname = pool_tmpjoin(pool, "pattern:", pool_id2str(pool, q2.elements[i + 1]), 0);
      unescape(newname);
      name = pool_str2id(pool, newname, 0);
      if (name)
	{
	  /* check if we already have that pattern */
	  for (j = 0; j < q.count; j++)
	    {
	      s2 = pool->solvables + q.elements[j];
	      if (s2->name == name && s2->arch == s->arch && s2->evr == s->evr)
		break;
	    }
	  if (j < q.count)
	    continue;	/* yes, do not add again */
	}
      /* new pattern */
      if (!name)
        name = pool_str2id(pool, newname, 1);
      if (!data)
	{
	  repo_internalize(repo);	/* to make that the lookups work */
	  data = repo_add_repodata(repo, flags);
	}
      s2 = pool_id2solvable(pool, repo_add_solvable(repo));
      s = pool->solvables + q2.elements[i];	/* re-calc pointer */
      s2->name = name;
      s2->arch = s->arch;
      s2->evr = s->evr;
      s2->vendor = s->vendor;
      /* add link requires */
      s2->requires = repo_addid_dep(repo, s2->requires, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1) , 0);
      /* add autopattern provides */
      if (!autopattern_id)
	autopattern_id = pool_str2id(pool, "autopattern()", 1);
      s2->provides = repo_addid_dep(repo, s2->provides, pool_rel2id(pool, autopattern_id, s->name, REL_EQ, 1), 0);
      /* add self provides */
      s2->provides = repo_addid_dep(repo, s2->provides, pool_rel2id(pool, s2->name, s2->evr, REL_EQ, 1), 0);
      if ((num = solvable_lookup_num(s, SOLVABLE_INSTALLTIME, 0)) != 0)
	repodata_set_num(data, s2 - pool->solvables, SOLVABLE_INSTALLTIME, num);
      if ((num = solvable_lookup_num(s, SOLVABLE_BUILDTIME, 0)) != 0)
	repodata_set_num(data, s2 - pool->solvables, SOLVABLE_BUILDTIME, num);
      if ((str = solvable_lookup_str(s, SOLVABLE_SUMMARY)) != 0)
	repodata_set_str(data, s2 - pool->solvables, SOLVABLE_SUMMARY, str);
      if ((str = solvable_lookup_str(s, SOLVABLE_DESCRIPTION)) != 0)
	repodata_set_str(data, s2 - pool->solvables, SOLVABLE_DESCRIPTION, str);
      /* fill in stuff from provides */
      prvp = repo->idarraydata + s->provides;
      while ((prv = *prvp++) != 0)            /* go through all provides */
	{
	  Id evr = 0;
	  if (ISRELDEP(prv))
	    {
	      Reldep *rd = GETRELDEP(pool, prv);
	      if (rd->flags != REL_EQ)
	        continue;
	      prv = rd->name;
	      evr = rd->evr;
	    }
	  pn = pool_id2str(pool, prv);
	  if (strncmp("pattern-", pn, 8) != 0)
	    continue;
	  newname = 0;
	  if (evr)
	    {
	      newname = pool_tmpjoin(pool, pool_id2str(pool, evr), 0, 0);
	      unescape(newname);
	    }
	  if (!strncmp(pn, "pattern-category(", 17) && evr)
	    {
	      char lang[9];
	      int l = strlen(pn);
	      Id langtag;
	      if (l > 17 + 9 || pn[l - 1] != ')')
		continue;
              strncpy(lang, pn + 17, l - 17 - 1);
	      lang[l - 17 - 1] = 0;
	      langtag = SOLVABLE_CATEGORY;
	      if (*lang && strcmp(lang, "en") != 0)
		langtag = pool_id2langid(pool, SOLVABLE_CATEGORY, lang, 1);
	      repodata_set_str(data, s2 - pool->solvables, langtag, newname);
	    }
	  else if (!strcmp(pn, "pattern-includes()") && evr)
	    repodata_add_poolstr_array(data, s2 - pool->solvables, SOLVABLE_INCLUDES, pool_tmpjoin(pool, "pattern:", newname, 0));
	  else if (!strcmp(pn, "pattern-extends()") && evr)
	    repodata_add_poolstr_array(data, s2 - pool->solvables, SOLVABLE_EXTENDS, pool_tmpjoin(pool, "pattern:", newname, 0));
	  else if (!strcmp(pn, "pattern-icon()") && evr)
	    repodata_set_str(data, s2 - pool->solvables, SOLVABLE_ICON, newname);
	  else if (!strcmp(pn, "pattern-order()") && evr)
	    repodata_set_str(data, s2 - pool->solvables, SOLVABLE_ORDER, newname);
	  else if (!strcmp(pn, "pattern-visible()") && !evr)
	    repodata_set_void(data, s2 - pool->solvables, SOLVABLE_ISVISIBLE);
	}
    }
  queue_free(&q);
  queue_free(&q2);
  if (data && !(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  else if (!data && !(flags & REPO_NO_INTERNALIZE))
    repo_internalize(repo);
  return 0;
}

