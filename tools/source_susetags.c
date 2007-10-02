#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "source_susetags.h"

#define PACK_BLOCK 255

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

struct deps {
  unsigned int provides;
  unsigned int requires;
  unsigned int obsoletes;
  unsigned int conflicts;
  unsigned int recommends;
  unsigned int supplements;
  unsigned int enhances;
  unsigned int suggests;
  unsigned int freshens;
};

struct parsedata {
  char *kind;
  Source *source;
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
  return source_addid_dep(pd->source, olddeps, id, isreq);
}

Source *
pool_addsource_susetags(Pool *pool, FILE *fp)
{
  char *line, *linep;
  int aline;
  Source *source;
  Solvable *s;
  struct deps *deps = 0, *dp = 0;
  int intag = 0;
  int cummulate = 0;
  int pack;
  char *sp[5];
  int i;
  struct parsedata pd;

  source = pool_addsource_empty(pool);
  memset(&pd, 0, sizeof(pd));
  line = malloc(1024);
  aline = 1024;

  pd.source = source;

  linep = line;
  pack = 0;
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
	  line[0] = '=';
	  line[intag + 2] = ' ';
	  linep = line + intag + 3;
	  continue;
	}
      if (*line == '#' || !*line)
	continue;
      if (!strncmp(line, "=Pkg:", 5) || !strncmp(line, "=Pat:", 5))
	{
	  if (dp)
	    dp->provides = source_addid_dep(source, dp->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	  pd.kind = 0;
	  if (line[3] == 't')
	    pd.kind = "pattern";
	  if ((pack & PACK_BLOCK) == 0)
	    {
	      pool->solvables = realloc(pool->solvables, (pool->nsolvables + pack + PACK_BLOCK + 1) * sizeof(Solvable));
	      memset(pool->solvables + source->start + pack, 0, (PACK_BLOCK + 1) * sizeof(Solvable));
	      if (!deps)
		deps = malloc((pack + PACK_BLOCK + 1) * sizeof(struct deps));
	      else
		deps = realloc(deps, (pack + PACK_BLOCK + 1) * sizeof(struct deps));
	      memset(deps + pack, 0, (PACK_BLOCK + 1) * sizeof(struct deps));
	    }
	  s = pool->solvables + source->start + pack;
	  dp = deps + pack;
	  pack++;
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
	  continue;
	}
      if (!strncmp(line, "=Prv:", 5))
	{
	  dp->provides = adddep(pool, &pd, dp->provides, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Req:", 5))
	{
	  dp->requires = adddep(pool, &pd, dp->requires, line, 1, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Prq:", 5))
	{
	  if (pd.kind)
	    dp->requires = adddep(pool, &pd, dp->requires, line, 0, 0);
	  else
	    dp->requires = adddep(pool, &pd, dp->requires, line, 2, 0);
	  continue;
	}
      if (!strncmp(line, "=Obs:", 5))
	{
	  dp->obsoletes = adddep(pool, &pd, dp->obsoletes, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Con:", 5))
	{
	  dp->conflicts = adddep(pool, &pd, dp->conflicts, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Rec:", 5))
	{
	  dp->recommends = adddep(pool, &pd, dp->recommends, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Sup:", 5))
	{
	  dp->supplements = adddep(pool, &pd, dp->supplements, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Enh:", 5))
	{
	  dp->enhances = adddep(pool, &pd, dp->enhances, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Sug:", 5))
	{
	  dp->suggests = adddep(pool, &pd, dp->suggests, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Fre:", 5))
	{
	  dp->freshens = adddep(pool, &pd, dp->freshens, line, 0, pd.kind);
	  continue;
	}
      if (!strncmp(line, "=Prc:", 5))
	{
	  dp->recommends = adddep(pool, &pd, dp->recommends, line, 0, 0);
	  continue;
	}
      if (!strncmp(line, "=Psg:", 5))
	{
	  dp->suggests = adddep(pool, &pd, dp->suggests, line, 0, 0);
	  continue;
	}
    }
  if (dp && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    dp->provides = source_addid_dep(source, dp->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
    
  pool->nsolvables += pack;
  source->nsolvables = pack;
  s = pool->solvables + source->start;
  for (i = 0; i < pack; i++, s++)
    {
      if (deps[i].provides)
        s->provides = source->idarraydata + deps[i].provides;
      if (deps[i].requires)
        s->requires = source->idarraydata + deps[i].requires;
      if (deps[i].conflicts)
        s->conflicts = source->idarraydata + deps[i].conflicts;
      if (deps[i].obsoletes)
        s->obsoletes = source->idarraydata + deps[i].obsoletes;
      if (deps[i].recommends)
        s->recommends = source->idarraydata + deps[i].recommends;
      if (deps[i].supplements)
        s->supplements = source->idarraydata + deps[i].supplements;
      if (deps[i].suggests)
        s->suggests = source->idarraydata + deps[i].suggests;
      if (deps[i].enhances)
        s->enhances = source->idarraydata + deps[i].enhances;
      if (deps[i].freshens)
        s->freshens = source->idarraydata + deps[i].freshens;
    }
  free(deps);
  if (pd.tmp)
    free(pd.tmp);
  free(line);
  return source;
}
