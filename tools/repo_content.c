/*
 * repo_content.c
 * 
 * Parses 'content' file into .solv
 * See http://en.opensuse.org/Standards/YaST2_Repository_Metadata/content for a description
 * of the syntax
 * 
 * 
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
#include "repo.h"
#include "util.h"
#include "repo_content.h"

/*
 * split l into m parts, store to sp[]
 *  split at whitespace
 */

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

/*
 * dependency relations
 */

static char *flagtab[] = {
  ">",
  "=",
  ">=",
  "<",
  "!=",
  "<="
};


/*
 * join up to three strings into one
 */

static char *
join(struct parsedata *pd, const char *s1, const char *s2, const char *s3)
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
      pd->tmp = sat_realloc(pd->tmp, pd->tmpl);
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


/*
 * add dependency to pool
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, Id marker)
{
  int flags, words;
  Id id, evrid;
  char *sp[4];

  words = 0;
  while (1)
    {
      /* Name [relop evr] [rest] --> 1, 2, 3 or 4 fields.  */
      if ( line )
        {
          words += split(line, sp + words, 4 - words);
          line = 0;
        }
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
	    line = sp[3];
          words = 0;
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
      olddeps = repo_addid_dep(pd->repo, olddeps, id, marker);
      if (! ( line || words > 0 ) )
        break;
    }
  return olddeps;
}


/*
 * split value and add to pool
 */

static void
add_multiple_strings(Repodata *data, Id handle, Id name, char *value)
{
  char *sp[2];
  while (value)
    {
      int words = split(value, sp, 2);
      if (!words)
	break;
      repodata_add_poolstr_array(data, handle, name, sp[0]);
      if (words == 1)
	break;
      value = sp[1];
    }
}


/*
 * add 'content' to repo
 *
 */

void
repo_add_content(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s, *firsts = 0;
  struct parsedata pd;
  Repodata *data;
  Id handle = 0;
  int contentstyle = 0;
  char *product_name = 0;
  char *product_version = 0;
  
  memset(&pd, 0, sizeof(pd));
  line = sat_malloc(1024);
  aline = 1024;

  if (repo->nrepodata)
    /* use last repodata */
    data = repo->repodata + repo->nrepodata - 1;
  else
    data = repo_add_repodata(repo, 0);

  pd.repo = repo;
  linep = line;
  s = 0;

  for (;;)
    {
      char *fields[2];
      
      /* read line into big-enough buffer */
      if (linep - line + 16 > aline)
	{
	  aline = linep - line;
	  line = sat_realloc(line, aline + 512);
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
      
      /* expect "key value" lines */
      if (split (line, fields, 2) == 2)
        {
	  char *key = fields[0];
	  char *value = fields[1];
#if 0
	  fprintf (stderr, "key %s, value %s\n", key, fields[1]);
#endif

#define istag(x) (!strcmp (key, x))
#define code10 (contentstyle == 10)
#define code11 (contentstyle == 11)
	  
	  if (contentstyle == 0) 
	    {
	      if (istag ("CONTENTSTYLE"))
		{
		  contentstyle = atoi(value);
		  continue;
		}
	      else
		contentstyle = 10;
	    }

	  if (code11 && istag ("REFERENCES"))
	    {
	      char *vals[3];
	      Id nameid;
	      Id evrid = 0;
	      
	      if (split(value, vals, 3) == 3)
		{
		  if (!strcmp(vals[1], "=")) 
		    {
		      nameid = str2id(pool, vals[0], 1);
		      evrid = str2id(pool, vals[2], 1);
		  
		      s = pool_id2solvable(pool, repo_add_solvable(repo));
		      repodata_extend(data, s - pool->solvables);
		      handle = repodata_get_handle(data, s - pool->solvables - repo->start);

		      s->name = nameid;
		      s->evr = evrid;
		      s->provides = adddep(pool, &pd, s->provides, "product()", 0);

		      continue;
		    }
		}
	      fprintf(stderr, "REFERENCES must be 'name = evr'\n");
	      break;
	    }
	  
	  if (code10 && istag ("PRODUCT"))
	    {
	      /* Finish old solvable, but only if it wasn't created
	         on demand without seeing a PRODUCT entry.  */
	      if (!firsts)
		{
		  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
		    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
		  if (s)
		    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements, 0);
		  /* Only support one product.  */
		  s = pool_id2solvable(pool, repo_add_solvable(repo));
		  repodata_extend(data, s - pool->solvables);
		  handle = repodata_get_handle(data, s - pool->solvables - repo->start);
		}
	      firsts = 0;
	      s->name = str2id(pool, join(&pd, "product", ":", value), 1);
	      continue;
	    }

	  /* Sometimes PRODUCT is not the first entry, but we need a solvable
	     from here on.  */
	  if (!s)
	    {
	      firsts = s = pool_id2solvable(pool, repo_add_solvable(repo));
	      repodata_extend(data, s - pool->solvables);
	      handle = repodata_get_handle(data, s - pool->solvables - repo->start);
	    }
	  if (istag ("VERSION"))
	    {
	      if (code11)
		{
		  repo_set_str(repo, s - pool->solvables, PRODUCT_VERSION, value);
		  product_version = strdup(value);
		}
	      else
		/* without a release? but that's like zypp implements it */
		s->evr = makeevr(pool, value);
	    }
	  else if (code11 && istag ("NAME"))
	    {
	      repo_set_str(repo, s - pool->solvables, PRODUCT_NAME, value);
	      product_name = strdup(value);
	    }
	  else if (code11 && istag ("DISTRIBUTION"))
	    repo_set_str(repo, s - pool->solvables, PRODUCT_DISTRIBUTION, value);
	  else if (code11 && istag ("FLAVOR"))
	    repo_set_str(repo, s - pool->solvables, PRODUCT_FLAVOR, value);
	  else if (istag ("DATADIR"))
	    repo_set_str(repo, s - pool->solvables, SUSETAGS_DATADIR, value);
	  else if (istag ("UPDATEURLS"))
	    add_multiple_strings(data, handle, PRODUCT_UPDATEURLS, value);
	  else if (istag ("EXTRAURLS"))
	    add_multiple_strings(data, handle, PRODUCT_EXTRAURLS, value);
	  else if (istag ("OPTIONALURLS"))
	    add_multiple_strings(data, handle, PRODUCT_OPTIONALURLS, value);
	  else if (istag ("SHORTLABEL"))
	    repo_set_str(repo, s - pool->solvables, PRODUCT_SHORTLABEL, value);
	  else if (istag ("LABEL")) /* LABEL is the products SUMMARY. */
	    repo_set_str(repo, s - pool->solvables, SOLVABLE_SUMMARY, value);
	  else if (!strncmp (key, "LABEL.", 6))
	    repo_set_str(repo, s - pool->solvables, pool_id2langid(pool, SOLVABLE_SUMMARY, key + 6, 1), value);
	  else if (istag ("FLAGS"))
	    add_multiple_strings(data, handle, PRODUCT_FLAGS, value);
	  else if (istag ("RELNOTESURL"))
	    repodata_add_poolstr_array(data, handle, PRODUCT_RELNOTESURL, value);
	  else if (istag ("VENDOR"))
	    {
	      if (code11)
		repo_set_str(repo, s - pool->solvables, PRODUCT_VENDOR, value);
	      else
		s->vendor = str2id(pool, value, 1);
	    }
	  
	  /*
	   * Every tag below is Code10 only
	   * 
	   */
	  
	  else if (code10 && istag ("DISTPRODUCT"))
	    /* DISTPRODUCT is for registration and Yast, not for the solver. */
	    repo_set_str(repo, s - pool->solvables, PRODUCT_DISTPRODUCT, value);
	  else if (code10 && istag ("DISTVERSION"))
	    /* DISTVERSION is for registration and Yast, not for the solver. */
	    repo_set_str(repo, s - pool->solvables, PRODUCT_DISTVERSION, value);
	  else if (code10 && istag ("ARCH"))
	    /* Theoretically we want to have the best arch of the given
	       modifiers which still is compatible with the system
	       arch.  We don't know the latter here, though.  */
	    s->arch = ARCH_NOARCH;
	  else if (code10 && istag ("PREREQUIRES"))
	    s->requires = adddep(pool, &pd, s->requires, value, SOLVABLE_PREREQMARKER);
	  else if (code10 && istag ("REQUIRES"))
	    s->requires = adddep(pool, &pd, s->requires, value, -SOLVABLE_PREREQMARKER);
	  else if (code10 && istag ("PROVIDES"))
	    s->provides = adddep(pool, &pd, s->provides, value, 0);
	  else if (code10 && istag ("CONFLICTS"))
	    s->conflicts = adddep(pool, &pd, s->conflicts, value, 0);
	  else if (code10 && istag ("OBSOLETES"))
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, value, 0);
	  else if (code10 && istag ("RECOMMENDS"))
	    s->recommends = adddep(pool, &pd, s->recommends, value, 0);
	  else if (code10 && istag ("SUGGESTS"))
	    s->suggests = adddep(pool, &pd, s->suggests, value, 0);
	  else if (code10 && istag ("SUPPLEMENTS"))
	    s->supplements = adddep(pool, &pd, s->supplements, value, 0);
	  else if (code10 && istag ("ENHANCES"))
	    s->enhances = adddep(pool, &pd, s->enhances, value, 0);
	  /* FRESHENS doesn't seem to exist.  */
	  else if (code10 && istag ("TYPE"))
	    repo_set_str(repo, s - pool->solvables, PRODUCT_TYPE, value);

	  /* XXX do something about LINGUAS and ARCH?
          * <ma>: Don't think so. zypp does not use or propagate them.
          */
#undef istag
	}
      else
	fprintf (stderr, "malformed line: %s\n", line);
    }

  if (!s)
    {
      fprintf(stderr, "No product solvable created !\n");
      exit(1);
    }
  if (code11)
    {
      if (!product_name) 
        {
	  fprintf(stderr, "Product must have a name !\n");
	  exit(1);
	}
      if (!product_version) 
        {
	  fprintf(stderr, "Product must have a version !\n");
	  exit(1);
	}
      const char *product = join(&pd, "product(", product_name, ")");
      s->provides = adddep(pool, &pd, s->provides, join(&pd, product, " = ", product_version), 0);
      free(product_version);
      free(product_name);
    }

  if (code10)
    {
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
      s->supplements = repo_fix_legacy(repo, s->provides, s->supplements, 0);
    }
  
  if (pd.tmp)
    sat_free(pd.tmp);
  sat_free(line);
}
