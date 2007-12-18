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

#include "pool.h"
#include "repo.h"
#include "attr_store.h"
#include "repo_susetags.h"

static int
split(char *l, char **sp, int m)
{
  int i;
  for (i = 0; i < m;)
    {
      while (*l == ' ')
	l++;
      if (!*l)
	break;
      sp[i++] = l;
      while (*l && *l != ' ')
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
  char **sources;
  int nsources;
  int last_found_source;
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
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, int isreq, char *kind)
{
  int i, flags;
  Id id, evrid;
  char *sp[4];

  i = split(line + 5, sp, 4);
  if (i != 1 && i != 3)
    {
      fprintf(stderr, "Bad dependency line: %s\n", line);
      exit(1);
    }
  if (kind)
    id = str2id(pool, join(pd, kind, ":", sp[0]), 1);
  else
    id = str2id(pool, sp[0], 1);
  if (i == 3)
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
    }
  return repo_addid_dep(pd->repo, olddeps, id, isreq);
}

Attrstore *attr;

static void
add_location (char *line, Solvable *s, unsigned entry)
{
  Pool *pool = s->repo->pool;
  char *sp[3];
  int i;

  i = split(line, sp, 3);
  if (i != 2 && i != 3)
    {
      fprintf(stderr, "Bad location line: %s\n", line);
      exit(1);
    }
  /* If we have a dirname, let's see if it's the same as arch.  In that
     case don't store it.  */
  if (i == 3 && !strcmp (sp[2], id2str (pool, s->arch)))
    sp[2] = 0, i = 2;
  if (i == 3 && sp[2])
    {
      /* medianr filename dir
         don't optimize this one */
      add_attr_special_int (attr, entry, str2id (pool, "medianr", 1), atoi (sp[0]));
      add_attr_localids_id (attr, entry, str2id (pool, "mediadir", 1), str2localid (attr, sp[2], 1));
      add_attr_string (attr, entry, str2id (pool, "mediafile", 1), sp[1]);
      return;
    }
  else
    {
      /* Let's see if we can optimize this a bit.  If the media file name
         can be formed by the base rpm information we don't store it, but
	 only a flag that we've seen it.  */
      unsigned int medianr = atoi (sp[0]);
      const char *n1 = sp[1];
      const char *n2 = id2str (pool, s->name);
      for (n2 = id2str (pool, s->name); *n2; n1++, n2++)
        if (*n1 != *n2)
	  break;
      if (*n2 || *n1 != '-')
        goto nontrivial;

      n1++;
      for (n2 = id2str (pool, s->evr); *n2; n1++, n2++)
	if (*n1 != *n2)
	  break;
      if (*n2 || *n1 != '.')
        goto nontrivial;
      n1++;
      for (n2 = id2str (pool, s->arch); *n2; n1++, n2++)
	if (*n1 != *n2)
	  break;
      if (*n2 || strcmp (n1, ".rpm"))
        goto nontrivial;
      add_attr_special_int (attr, entry, str2id (pool, "medianr", 1), medianr);
      add_attr_void (attr, entry, str2id (pool, "mediafile", 1));
      return;

nontrivial:
      add_attr_special_int (attr, entry, str2id (pool, "medianr", 1), medianr);
      add_attr_string (attr, entry, str2id (pool, "mediafile", 1), sp[1]);
      return;
    }
}

static void
add_source (char *line, struct parsedata *pd, Solvable *s, unsigned entry, int first)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  char *sp[5];

  if (split(line, sp, 5) != 4)
    {
      fprintf(stderr, "Bad source line: %s\n", line);
      exit(1);
    }

  Id name = str2id(pool, sp[0], 1);
  Id evr = makeevr(pool, join(pd, sp[1], "-", sp[2]));
  Id arch = str2id(pool, sp[3], 1);

  /* Now, if the source of a package only differs in architecture
     (src or nosrc), code only that fact.  */
  if (s->name == name && s->evr == evr
      && (arch == ARCH_SRC || arch == ARCH_NOSRC))
    add_attr_void (attr, entry,
                   str2id (pool, arch == ARCH_SRC ? "source" : "nosource", 1));
  else if (first)
    {
      if (entry >= pd->nsources)
        {
	  if (pd->nsources)
	    {
	      pd->sources = realloc (pd->sources, (entry + 256) * sizeof (*pd->sources));
	      memset (pd->sources + pd->nsources, 0, (entry + 256 - pd->nsources) * sizeof (*pd->sources));
	    }
	  else
	    pd->sources = calloc (entry + 256, sizeof (*pd->sources));
	  pd->nsources = entry + 256;
	}
      /* Uarrr.  Unsplit.  */
      sp[0][strlen (sp[0])] = ' ';
      sp[1][strlen (sp[1])] = ' ';
      sp[2][strlen (sp[2])] = ' ';
      pd->sources[entry] = strdup (sp[0]);
    }
  else
    {
      unsigned n, nn;
      Solvable *found = 0;
      /* Otherwise we may find a solvable with exactly matching name, evr, arch
         in the repository already.  In that case encode its ID.  */
      for (n = repo->start, nn = repo->start + pd->last_found_source;
           n < repo->end; n++, nn++)
        {
	  if (nn >= repo->end)
	    nn = repo->start;
	  found = pool->solvables + nn;
	  if (found->repo == repo
	      && found->name == name
	      && found->evr == evr
	      && found->arch == arch)
	    {
	      pd->last_found_source = nn - repo->start;
	      break;
	    }
        }
      if (n != repo->end)
        add_attr_intlist_int (attr, entry, str2id (pool, "sourceid", 1), nn - repo->start);
      else
        {
          add_attr_localids_id (attr, entry, str2id (pool, "source", 1), str2localid (attr, sp[0], 1));
          add_attr_localids_id (attr, entry, str2id (pool, "source", 1), str2localid (attr, join (pd, sp[1], "-", sp[2]), 1));
          add_attr_localids_id (attr, entry, str2id (pool, "source", 1), str2localid (attr, sp[3], 1));
	}
    }
}

void
repo_add_susetags(Repo *repo, FILE *fp, Id vendor, int with_attr)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  Solvable *s;
  int intag = 0;
  int cummulate = 0;
  int indesc = 0;
  int last_found_pack = 0;
  char *sp[5];
  struct parsedata pd;

  if (with_attr)
    attr = new_store(pool);
  memset(&pd, 0, sizeof(pd));
  line = malloc(1024);
  aline = 1024;

  pd.repo = repo;

  linep = line;
  s = 0;

  for (;;)
    {
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
      if (intag)
	{
	  int isend = linep[-intag - 2] == '-' && linep[-1] == ':' && !strncmp(linep - 1 - intag, line + 1, intag) && (linep == line + 1 + intag + 1 + 1 + 1 + intag + 1 || linep[-intag - 3] == '\n');
	  if (cummulate && !isend)
	    {
	      *linep++ = '\n';
	      continue;
	    }
	  if (cummulate && isend)
	    {
	      linep[-intag - 2] = 0;
	      if (linep[-intag - 3] == '\n')
	        linep[-intag - 3] = 0;
	      linep = line;
	      intag = 0;
	    }
	  if (!cummulate && isend)
	    {
	      intag = 0;
	      linep = line;
	      continue;
	    }
	  if (!cummulate && !isend)
	    linep = line + intag + 3;
	}
      else
	linep = line;
      if (!intag && line[0] == '+' && line[1] && line[1] != ':')
	{
	  char *tagend = strchr(line, ':');
	  if (!tagend)
	    {
	      fprintf(stderr, "bad line: %s\n", line);
	      exit(1);
	    }
	  intag = tagend - (line + 1);
	  if (!strncmp (line, "+Des:", 5)
	      || !strncmp (line, "+Eul:", 5)
	      || !strncmp (line, "+Ins:", 5)
	      || !strncmp (line, "+Del:", 5)
	      || !strncmp (line, "+Aut:", 5))
	    cummulate = 1;
	  else
	    cummulate = 0;
	  line[0] = '=';
	  line[intag + 2] = ' ';
	  linep = line + intag + 3;
	  continue;
	}
      if (*line == '#' || !*line)
	continue;
      if (indesc < 2
          && (!strncmp(line, "=Pkg:", 5) || !strncmp(line, "=Pat:", 5)))
	{
	  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	  if (s)
	    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
	  pd.kind = 0;
	  if (line[3] == 't')
	    pd.kind = "pattern";
	  s = pool_id2solvable(pool, repo_add_solvable(repo));
	  last_found_pack = (s - pool->solvables) - repo->start;
          if (split(line + 5, sp, 5) != 4)
	    {
	      fprintf(stderr, "Bad line: %s\n", line);
	      exit(1);
	    }
	  if (pd.kind)
	    s->name = str2id(pool, join(&pd, pd.kind, ":", sp[0]), 1);
	  else
	    s->name = str2id(pool, sp[0], 1);
	  s->evr = makeevr(pool, join(&pd, sp[1], "-", sp[2]));
	  s->arch = str2id(pool, sp[3], 1);
	  s->vendor = vendor;
	  continue;
	}
      if (indesc == 2
          && (!strncmp(line, "=Pkg:", 5) || !strncmp(line, "=Pat:", 5)))
	{
	  Id name, evr, arch;
	  int n, nn;
	  pd.kind = 0;
	  if (line[3] == 't')
	    pd.kind = "pattern";
          if (split(line + 5, sp, 5) != 4)
	    {
	      fprintf(stderr, "Bad line: %s\n", line);
	      exit(1);
	    }
	  s = 0;
	  if (pd.kind)
	    name = str2id(pool, join(&pd, pd.kind, ":", sp[0]), 0);
	  else
	    name = str2id(pool, sp[0], 0);
	  evr = makeevr(pool, join(&pd, sp[1], "-", sp[2]));
	  arch = str2id(pool, sp[3], 0);
	  /* If we found neither the name or the arch at all in this repo
	     there's no chance of finding the exact solvable either.  */
	  if (!name || !arch)
	    continue;
	  /* Now look for a solvable with the given name,evr,arch.
	     Our input is structured so, that the second set of =Pkg
	     lines comes in roughly the same order as the first set, so we 
	     have a hint at where to start our search, namely were we found
	     the last entry.  */
	  for (n = repo->start, nn = n + last_found_pack; n < repo->end; n++, nn++)
	    {
	      if (nn >= repo->end)
	        nn = repo->start;
	      s = pool->solvables + nn;
	      if (s->repo == repo && s->name == name && s->evr == evr && s->arch == arch)
	        break;
	    }
	  if (n == repo->end)
	    s = 0;
	  else
	    last_found_pack = nn - repo->start;
	  continue;
	}
      /* If we have no current solvable to add to, ignore all further lines
         for it.  Probably invalid input data in the second set of
	 solvables.  */
      if (indesc >= 2 && !s)
        {
	  fprintf (stderr, "Huh?\n");
          continue;
	}
      if (!strncmp(line, "=Prv:", 5))
	{
	  s->provides = adddep(pool, &pd, s->provides, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Req:", 5))
	{
	  s->requires = adddep(pool, &pd, s->requires, line, 1, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Prq:", 5))
	{
	  if (pd.kind)
	    s->requires = adddep(pool, &pd, s->requires, line, 0, 0);
	  else
	    s->requires = adddep(pool, &pd, s->requires, line, 2, 0);
	  continue;
	}
      if (!strncmp(line, "=Obs:", 5))
	{
	  s->obsoletes = adddep(pool, &pd, s->obsoletes, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Con:", 5))
	{
	  s->conflicts = adddep(pool, &pd, s->conflicts, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Rec:", 5))
	{
	  s->recommends = adddep(pool, &pd, s->recommends, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Sup:", 5))
	{
	  s->supplements = adddep(pool, &pd, s->supplements, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Enh:", 5))
	{
	  s->enhances = adddep(pool, &pd, s->enhances, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Sug:", 5))
	{
	  s->suggests = adddep(pool, &pd, s->suggests, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Fre:", 5))
	{
	  s->freshens = adddep(pool, &pd, s->freshens, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Prc:", 5))
	{
	  s->recommends = adddep(pool, &pd, s->recommends, line, 0, 0);
	  continue;
	}
      if (!strncmp(line, "=Psg:", 5))
	{
	  s->suggests = adddep(pool, &pd, s->suggests, line, 0, 0);
	  continue;
	}
      if (!with_attr)
        continue;
      if (!strncmp(line, "=Grp:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_localids_id (attr, last_found_pack, str2id (pool, "group", 1), str2localid (attr, line + 6, 1));
	  continue;
	}
      if (!strncmp(line, "=Lic:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_localids_id (attr, last_found_pack, str2id (pool, "license", 1), str2localid (attr, line + 6, 1));
	  continue;
	}
      if (!strncmp(line, "=Loc:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_location (line + 6, s, last_found_pack);
	  continue;
	}
      if (!strncmp(line, "=Src:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_source (line + 6, &pd, s, last_found_pack, 1);
	  continue;
	}
      if (!strncmp(line, "=Siz:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  if (split (line + 6, sp, 3) == 2)
	    {
	      add_attr_int (attr, last_found_pack, str2id (pool, "downloadsize", 1), (atoi (sp[0]) + 1023) / 1024);
	      add_attr_int (attr, last_found_pack, str2id (pool, "installsize", 1), (atoi (sp[1]) + 1023) / 1024);
	    }
	  continue;
	}
      if (!strncmp(line, "=Tim:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  unsigned int t = atoi (line + 6);
	  if (t)
	    add_attr_int (attr, last_found_pack, str2id (pool, "time", 1), t);
	  continue;
	}
      if (!strncmp(line, "=Kwd:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_localids_id (attr, last_found_pack, str2id (pool, "keywords", 1), str2localid (attr, line + 6, 1));
	  continue;
	}
      if (!strncmp(line, "=Aut:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_blob (attr, last_found_pack, str2id (pool, "authors", 1), line + 6, strlen (line + 6) + 1);
	  continue;
	}
      if (!strncmp(line, "=Sum:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_string (attr, last_found_pack, str2id (pool, "summary", 1), line + 6);
	  continue;
	}
      if (!strncmp(line, "=Des:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_blob (attr, last_found_pack, str2id (pool, "description", 1), line + 6, strlen (line + 6) + 1);
	  continue;
	}
      if (!strncmp(line, "=Eul:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_blob (attr, last_found_pack, str2id (pool, "eula", 1), line + 6, strlen (line + 6) + 1);
	  continue;
	}
      if (!strncmp(line, "=Ins:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_blob (attr, last_found_pack, str2id (pool, "messageins", 1), line + 6, strlen (line + 6) + 1);
	  continue;
	}
      if (!strncmp(line, "=Del:", 5))
        {
	  ensure_entry (attr, last_found_pack);
	  add_attr_blob (attr, last_found_pack, str2id (pool, "messagedel", 1), line + 6, strlen (line + 6) + 1);
	  continue;
	}
      if (!strncmp(line, "=Shr:", 5))
        {
	  /* XXX Not yet handled.  Two possibilities: either include all
	     referenced data verbatim here, or write out the sharing
	     information.  */
	  continue;
	}
      if (!strncmp(line, "=Ver:", 5))
	{
	  last_found_pack = 0;
	  indesc++;
	  continue;
	}
    }
  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
  if (s)
    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
    
  if (pd.sources)
    {
      int i;
      for (i = 0; i < pd.nsources; i++)
        if (pd.sources[i])
	  {
	    add_source (pd.sources[i], &pd, pool->solvables + repo->start + i, i, 0);
	    free (pd.sources[i]);
	  }
      free (pd.sources);
    }
  if (pd.tmp)
    free(pd.tmp);
  free(line);
}
