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
#include <assert.h>

#include "pool.h"
#include "util.h"
#include "repo_content.h"

#define PACK_BLOCK 16

static int
split(char *l, char **sp, int m)
{
  int i;
  for (i = 0; i < m;)
    {
      while (*l == ' ' || *l == '\t')
	l++;
      if (!*l)
	break;
      sp[i++] = l;
      if (i == m)
        break;
      while (*l && !(*l == ' ' || *l == '\t'))
	l++;
      if (!*l)
	break;
      *l++ = 0;
    }
  return i;
}

struct parsedata {
  char *kind;
  Repo *repo;
  char *tmp;
  int tmpl;
};

static Id
makeevr(Pool *pool, char *s)
{
  if (!strncmp(s, "0:", 2) && s[2])
    s += 2;
  return str2id(pool, s, 1);
}

static char *flagtab[] = {
  ">",
  "=",
  ">=",
  "<",
  "!=",
  "<="
};

static char *
join(struct parsedata *pd, char *s1, char *s2, char *s3)
{
  int l = 1;
  char *p;

  if (s1)
    l += strlen(s1);
  if (s2)
    l += strlen(s2);
  if (s3)
    l += strlen(s3);
  if (l > pd->tmpl)
    {
      pd->tmpl = l + 256;
      if (!pd->tmp)
	pd->tmp = malloc(pd->tmpl);
      else
	pd->tmp = realloc(pd->tmp, pd->tmpl);
    }
  p = pd->tmp;
  if (s1)
    {
      strcpy(p, s1);
      p += strlen(s1);
    }
  if (s2)
    {
      strcpy(p, s2);
      p += strlen(s2);
    }
  if (s3)
    {
      strcpy(p, s3);
      p += strlen(s3);
    }
  return pd->tmp;
}

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, int isreq)
{
  int flags, words;
  Id id, evrid;
  char *sp[4];

  words = 0;
  while (1)
    {
      /* Name [relop evr [rest]] --> 1, 3 or 4 fields.  */
      words += split(line, sp + words, 4 - words);
      if (words == 2)
	{
	  fprintf(stderr, "Bad dependency line: %s\n", line);
	  exit(1);
	}
      line = 0;
      /* Hack, as the content file adds 'package:' for package
         dependencies sometimes.  */
      if (!strncmp (sp[0], "package:", 8))
        sp[0] += 8;
      id = str2id(pool, sp[0], 1);
      if (words >= 3 && strpbrk (sp[1], "<>="))
	{
	  evrid = makeevr(pool, sp[2]);
	  for (flags = 0; flags < 6; flags++)
	    if (!strcmp(sp[1], flagtab[flags]))
	      break;
	  if (flags == 6)
	    {
	      fprintf(stderr, "Unknown relation '%s'\n", sp[1]);
	      exit(1);
	    }
	  id = rel2id(pool, id, evrid, flags + 1, 1);
	  /* Consume three words, there's nothing to move to front.  */
	  if (words == 4)
	    line = sp[3], words = 0;
	}
      else
        {
	  int j;
	  /* Consume one word.  If we had more move them to front.  */
	  words--;
	  for (j = 0; j < words; j++)
	    sp[j] = sp[j+1];
	  if (words == 3)
	    line = sp[2], words = 2;
	}
      olddeps = repo_addid_dep(pd->repo, olddeps, id, isreq);
      if (!line)
        break;
    }
  return olddeps;
}

void
repo_add_content(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s;
  int pack;
  struct parsedata pd;

  memset(&pd, 0, sizeof(pd));
  line = xmalloc(1024);
  aline = 1024;

  pd.repo = repo;
  linep = line;
  pack = 0;
  s = 0;

  if (!repo->start || repo->start == repo->end)
    repo->start = pool->nsolvables;
  repo->end = pool->nsolvables;

  for (;;)
    {
      char *fields[2];
      if (linep - line + 16 > aline)
	{
	  aline = linep - line;
	  line = realloc(line, aline + 512);
	  linep = line + aline;
	  aline += 512;
	}
      if (!fgets(linep, aline - (linep - line), fp))
	break;
      linep += strlen(linep);
      if (linep == line || linep[-1] != '\n')
        continue;
      *--linep = 0;
      linep = line;
      if (split (line, fields, 2) == 2)
        {
	  char *key = fields[0];
	  char *value = fields[1];
	  char *modifier = strchr (key, '.');
	  if (modifier)
	    *modifier++ = 0;
#if 0
	  if (modifier)
	    fprintf (stderr, "key %s, mod %s, value %s\n", key, modifier, fields[1]);
	  else
	    fprintf (stderr, "key %s, value %s\n", key, fields[1]);
#endif

#define istag(x) !strcmp (key, x)
	  if (istag ("PRODUCT"))
	    {
	      if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
		s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	      if (s)
		s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
	      /* Only support one product.  */
	      pd.kind = "product";
	      if ((pack & PACK_BLOCK) == 0)
		{
		  pool->solvables = realloc(pool->solvables, (pool->nsolvables + pack + PACK_BLOCK + 1) * sizeof(Solvable));
		  memset(pool->solvables + pool->nsolvables + pack, 0, (PACK_BLOCK + 1) * sizeof(Solvable));
		}
	      s = pool->solvables + pool->nsolvables + pack;
	      s->repo = repo;
	      s->name = str2id(pool, join(&pd, pd.kind, ":", value), 1);
	      pack++;
	    }
	  else if (istag ("VERSION"))
	    /* without a release? but that's like zypp implements it */
	    s->evr = makeevr(pool, value);
	  else if (istag ("DISTPRODUCT"))
	    ; /* DISTPRODUCT is only for Yast, not the package manager */
	  else if (istag ("DISTVERSION"))
	    ; /* DISTVERSION is only for Yast, not the package manager */
	  else if (istag ("VENDOR"))
	    s->vendor = str2id(pool, value, 1);
	  else if (istag ("ARCH"))
	    /* Theoretically we want to have the best arch of the given
	       modifiers which still is compatible with the system
	       arch.  We don't know the latter here, though.  */
	    s->arch = ARCH_NOARCH;
	  else if (istag ("PREREQUIRES"))
	    s->requires = adddep(pool, &pd, s->requires, value, 2);
	  else if (istag ("REQUIRES"))
	    s->requires = adddep(pool, &pd, s->requires, value, 1);
	  else if (istag ("PROVIDES"))
	    s->provides = adddep(pool, &pd, s->provides, value, 0);
	  else if (istag ("CONFLICTS"))
	    s->conflicts = adddep(pool, &pd, s->conflicts, value, 0);
	  else if (istag ("OBSOLETES"))
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, value, 0);
	  else if (istag ("RECOMMENDS"))
	    s->recommends = adddep(pool, &pd, s->recommends, value, 0);
	  else if (istag ("SUGGESTS"))
	    s->suggests = adddep(pool, &pd, s->suggests, value, 0);
	  else if (istag ("SUPPLEMENTS"))
	    s->supplements = adddep(pool, &pd, s->supplements, value, 0);
	  else if (istag ("ENHANCES"))
	    s->enhances = adddep(pool, &pd, s->enhances, value, 0);
	  /* FRESHENS doesn't seem to exist.  */
	  /* XXX do something about LINGUAS and ARCH? */
#undef istag
	}
      else
	fprintf (stderr, "malformed line: %s\n", line);
    }

  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  if (s)
    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
    
  pool->nsolvables += pack;
  repo->nsolvables += pack;
  repo->end += pack;
  if (pd.tmp)
    free(pd.tmp);
  free(line);
}
