/*
 * repo_products.c
 * 
 * Parses all files below 'proddir'
 * See http://en.opensuse.org/Product_Management/Code11
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
#include <dirent.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "repo_content.h"

struct parsedata {
  Repo *repo;
  char *tmp;
  int tmpl;
};



#if 0
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
#endif

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


#if 0
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
#endif

/*
 * add single product to repo
 *
 */

static void
repo_add_product(struct parsedata *pd, FILE *fp)
{
#if 0
  Pool *pool = pd->repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s, *firsts = 0;
  Id handle = 0;
  
  line = sat_malloc(1024);
  aline = 1024;

  linep = line;
  s = 0;

  for (;;)
    {
/*      char *fields[2]; */
      
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
	}
      else
	fprintf (stderr, "malformed line: %s\n", line);
    }

  if (!s)
    {
      fprintf(stderr, "No product solvable created !\n");
      exit(1);
    }

  if (!s->arch)
    s->arch = ARCH_NOARCH;
  if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    {
      s->provides = repo_addid_dep(pd->repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
    }
  
  sat_free(line);
#endif
}


/*
 * read all .prod files from directory
 * parse each one as a product
 */

void
repo_add_products(Repo *repo, const char *proddir)
{
  DIR *dir = opendir(proddir);
  struct dirent *entry;
  struct parsedata pd;
  Repodata *data;
  
  memset(&pd, 0, sizeof(pd));
  if (repo->nrepodata)
    /* use last repodata */
    data = repo->repodata + repo->nrepodata - 1;
  else
    data = repo_add_repodata(repo, 0);

  pd.repo = repo;

  if (!dir)
    {
      perror(proddir);
      return;
    }
  
  while ((entry = readdir(dir)))
    {
      const char *dot;
      dot = strrchr( entry->d_name, '.' );
      if (dot && strcmp(dot, ".prod") == 0)
	{
	  char *fullpath = join(&pd, proddir, "/", entry->d_name);
	  FILE *fp = fopen(fullpath, "r");
	  if (!fp)
	    {
	      perror(fullpath);
	      break;
	    }
	  repo_add_product(&pd, fp);
	  fclose(fp);
	}
    }
  if (pd.tmp)
    sat_free(pd.tmp);
  closedir(dir);
}
