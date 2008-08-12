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
  Id langcache[ID_NUM_INTERNAL];
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
 * create epoch:version-release as Id
 */

static Id
makeevr(Pool *pool, char *s)
{
  if (!strncmp(s, "0:", 2) && s[2])
    s += 2;
  return str2id(pool, s, 1);
}


/*
 * create localized tag
 */

static Id
langtag(struct parsedata *pd, Id tag, const char *language)
{
    if (language && !language[0])
        language = 0;
    if (!language || tag >= ID_NUM_INTERNAL)
        return pool_id2langid(pd->repo->pool, tag, language, 1);
    return pool_id2langid(pd->repo->pool, tag, language, 1);
    if (!pd->langcache[tag])
        pd->langcache[tag] = pool_id2langid(pd->repo->pool, tag, language, 1);
    return pd->langcache[tag];
}




enum sections 
{
  SECTION_UNKNOWN,
  SECTION_PRODUCT,
  SECTION_TRANSLATED,
  SECTION_UPDATE
};


/*
 * add single product to repo
 *
 */

static void
repo_add_product(struct parsedata *pd, Repodata *data, FILE *fp)
{
  Repo *repo = pd->repo;
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s = 0;
  Id handle = 0;

  enum sections current_section = SECTION_UNKNOWN;

  line = sat_malloc(1024);
  aline = 1024;

  linep = line;
  s = 0;

  for (;;)
    {      
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

      /*
       * Very trivial .ini parser
       */
      
      /* skip empty and comment lines */
      if (*linep == '#'
	  || *linep == 0)
	{
	  continue;
	}
      
      /* sections must start at column 0 */
      if (*linep == '[')
	{
	  char *secp = linep+1;
	  char *endp = linep;
	  endp = strchr(secp, ']');
	  if (!endp)
	    {
	      fprintf(stderr, "Skipping unclosed section '%s'\n", line);
	      continue;
	    }
	  *endp = 0;
	  if (!strcmp(secp, "product"))
	    current_section = SECTION_PRODUCT;
	  else if (!strcmp(secp, "translated"))
	    current_section = SECTION_TRANSLATED;
	  else if (!strcmp(secp, "update"))
	    current_section = SECTION_UPDATE;
	  else
	    {
	      fprintf(stderr, "Skipping unknown section '%s'\n", secp);
	      current_section = SECTION_UNKNOWN;
	    }
	  continue;
	}
      else if (current_section != SECTION_UNKNOWN)
	{
	  char *ptr = linep;
	  char *key, *value, *lang;

	  lang = 0;
	  
	  /* split line into '<key>[<lang>] = <value>' */
	  while (*ptr && (*ptr == ' ' || *ptr == '\t'))
	    ++ptr;
	  key = ptr;
	  while (*ptr && !(*ptr == ' ' || *ptr == '\t' || *ptr == '=' || *ptr == '['))
	    ++ptr;
	  if (*ptr == '[')
	    {
	      *ptr++ = 0;
	      lang = ptr;
	      while (*ptr && !(*ptr == ']'))
		++ptr;
	      *ptr++ = 0;
	    }
	  if (*ptr != '=')
	    *ptr++ = 0;
	  while (*ptr && !(*ptr == '='))
	    ++ptr;
	  if (*ptr == '=')
	    *ptr++ = 0;
	  while (*ptr && (*ptr == ' ' || *ptr == '\t'))
	    ++ptr;
	  value = ptr;

	  /*
	   * [product]
	   */
	  
	  if (current_section == SECTION_PRODUCT)
	    {
	      if (!s)
		{
		  s = pool_id2solvable(pool, repo_add_solvable(repo));
		  repodata_extend(data, s - pool->solvables);
		  handle = repodata_get_handle(data, s - pool->solvables - repo->start);
		}
	      if (!strcmp(key, "name"))
		  s->name = str2id(pool, join(pd, "product", ":", value), 1);
	      else if (!strcmp(key, "version"))
		s->evr = makeevr(pool, value);
	      else if (!strcmp(key, "vendor"))
		s->vendor = str2id(pool, value, 1);
	      else if (!strcmp(key, "distribution"))
		repo_set_str(repo, s - pool->solvables, SOLVABLE_DISTRIBUTION, value);
	      else if (!strcmp (key, "flavor"))
		repo_set_str(repo, s - pool->solvables, PRODUCT_FLAVOR, value);	    
	    }
	    /*
	     * [translated]
	     */
	  else if (current_section == SECTION_TRANSLATED)
	    {
	      if (!strcmp(key, "summary"))
		{
		  repodata_set_str(data, handle, langtag(pd, SOLVABLE_SUMMARY, lang), value );
		}
	      else if (!strcmp(key, "description"))
		repodata_set_str(data, handle, langtag(pd, SOLVABLE_DESCRIPTION, lang), value );
	    }
	    /*
	     * [update]
	     */
	  else if (current_section == SECTION_UPDATE)
	    {
	    }
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
}


/*
 * read all .prod files from directory
 * parse each one as a product
 */

void
repo_add_products(Repo *repo, Repodata *repodata, const char *proddir)
{
  DIR *dir = opendir(proddir);
  struct dirent *entry;
  struct parsedata pd;
  
  memset(&pd, 0, sizeof(pd));
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
	  repo_add_product(&pd, repodata, fp);
	  fclose(fp);
	}
    }
  if (pd.tmp)
    sat_free(pd.tmp);
  closedir(dir);
}
