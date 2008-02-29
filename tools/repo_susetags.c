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
#include "tools_util.h"
#include "repo_susetags.h"

struct parsedata {
  char *kind;
  Repo *repo;
  Repodata *data;
  struct parsedata_common common;
  int last_found_source;
  char **share_with;
  int nshare;
  Id (*dirs)[3]; // dirid, size, nfiles
  int ndirs;
};

static char *flagtab[] = {
  ">",
  "=",
  ">=",
  "<",
  "!=",
  "<="
};

/*
 * adddep
 * create and add dependency
 */

static unsigned int
adddep(Pool *pool, struct parsedata *pd, unsigned int olddeps, char *line, Id marker, char *kind)
{
  int i, flags;
  Id id, evrid;
  char *sp[4];

  i = split(line + 5, sp, 4); /* name, <op>, evr, ? */
  if (i != 1 && i != 3) /* expect either 'name' or 'name' <op> 'evr' */
    {
      fprintf(stderr, "Bad dependency line: %s\n", line);
      exit(1);
    }
  if (kind)
    id = str2id(pool, join2(kind, ":", sp[0]), 1);
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
  return repo_addid_dep(pd->repo, olddeps, id, marker);
}


/*
 * add_location
 * 
 */

static void
add_location(struct parsedata *pd, char *line, Solvable *s, unsigned entry)
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
      repodata_set_constant(pd->data, entry, SOLVABLE_MEDIANR, atoi(sp[0]));
      repodata_set_poolstr(pd->data, entry, SOLVABLE_MEDIADIR, sp[2]);
      repodata_set_str(pd->data, entry, SOLVABLE_MEDIAFILE, sp[1]);
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

      repodata_set_constant(pd->data, entry, SOLVABLE_MEDIANR, medianr);
      repodata_set_void(pd->data, entry, SOLVABLE_MEDIAFILE);
      return;

nontrivial:
      repodata_set_constant(pd->data, entry, SOLVABLE_MEDIANR, medianr);
      repodata_set_str(pd->data, entry, SOLVABLE_MEDIAFILE, sp[1]);
      return;
    }
}

/*
 * add_source
 * 
 */

static void
add_source(struct parsedata *pd, char *line, Solvable *s, unsigned entry)
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
  Id evr = makeevr(pool, join2(sp[1], "-", sp[2]));
  Id arch = str2id(pool, sp[3], 1);
  /* XXX: could record a dep here, depends on where we want to store the data */
  if (name == s->name)
    repodata_set_void(pd->data, entry, SOLVABLE_SOURCENAME);
  else
    repodata_set_id(pd->data, entry, SOLVABLE_SOURCENAME, name);
  if (evr == s->evr)
    repodata_set_void(pd->data, entry, SOLVABLE_SOURCEEVR);
  else
    repodata_set_id(pd->data, entry, SOLVABLE_SOURCEEVR, evr);
  repodata_set_constantid(pd->data, entry, SOLVABLE_SOURCEARCH, arch);
}
  
/*
 * add_dirline
 * add a line with directory information
 * 
 */

static void
add_dirline (struct parsedata *pd, char *line)
{
  char *sp[6];
  if (split (line, sp, 6) != 5)
    return;
  pd->dirs = sat_extend(pd->dirs, pd->ndirs, 1, sizeof(pd->dirs[0]), 31);
  long filesz = strtol (sp[1], 0, 0);
  filesz += strtol (sp[2], 0, 0);
  long filenum = strtol (sp[3], 0, 0);
  filenum += strtol (sp[4], 0, 0);
  /* hack: we know that there's room for a / */
  if (*sp[0] != '/')
    *--sp[0] = '/';
  unsigned dirid = repodata_str2dir(pd->data, sp[0], 1);
#if 0
fprintf(stderr, "%s -> %d\n", sp[0], dirid);
#endif
  pd->dirs[pd->ndirs][0] = dirid;
  pd->dirs[pd->ndirs][1] = filesz;
  pd->dirs[pd->ndirs][2] = filenum;
  pd->ndirs++;
}


/*
 * id3_cmp
 * compare 
 * 
 */

static int
id3_cmp (const void *v1, const void *v2)
{
  Id *i1 = (Id*)v1;
  Id *i2 = (Id*)v2;
  return i1[0] - i2[0];
}


/*
 * commit_diskusage
 * 
 */

static void
commit_diskusage (struct parsedata *pd, unsigned entry)
{
  unsigned i;
  Dirpool *dp = &pd->data->dirpool;
  /* Now sort in dirid order.  This ensures that parents come before
     their children.  */
  if (pd->ndirs > 1)
    qsort (pd->dirs, pd->ndirs, sizeof (pd->dirs[0]), id3_cmp);
  /* Substract leaf numbers from all parents to make the numbers
     non-cumulative.  This must be done post-order (i.e. all leafs
     adjusted before parents).  We ensure this by starting at the end of
     the array moving to the start, hence seeing leafs before parents.  */
  for (i = pd->ndirs; i--;)
    {
      unsigned p = dirpool_parent(dp, pd->dirs[i][0]);
      unsigned j = i;
      for (; p; p = dirpool_parent(dp, p))
        {
          for (; j--;)
	    if (pd->dirs[j][0] == p)
	      break;
	  if (j < pd->ndirs)
	    {
	      if (pd->dirs[j][1] < pd->dirs[i][1])
	        pd->dirs[j][1] = 0;
	      else
	        pd->dirs[j][1] -= pd->dirs[i][1];
	      if (pd->dirs[j][2] < pd->dirs[i][2])
	        pd->dirs[j][2] = 0;
	      else
	        pd->dirs[j][2] -= pd->dirs[i][2];
	    }
	  else
	    /* Haven't found this parent in the list, look further if
	       we maybe find the parents parent.  */
	    j = i;
	}
    }
#if 0
  char sbuf[1024];
  char *buf = sbuf;
  unsigned slen = sizeof (sbuf);
  for (i = 0; i < pd->ndirs; i++)
    {
      dir2str (attr, pd->dirs[i][0], &buf, &slen);
      fprintf (stderr, "have dir %d %d %d %s\n", pd->dirs[i][0], pd->dirs[i][1], pd->dirs[i][2], buf);
    }
  if (buf != sbuf)
    free (buf);
#endif
  for (i = 0; i < pd->ndirs; i++)
    if (pd->dirs[i][1] || pd->dirs[i][2])
      {
	repodata_add_dirnumnum(pd->data, entry, SOLVABLE_DISKUSAGE, pd->dirs[i][0], pd->dirs[i][1], pd->dirs[i][2]);
      }
  pd->ndirs = 0;
}


/* Unfortunately "a"[0] is no constant expression in the C languages,
   so we need to pass the four characters individually :-/  */
#define CTAG(a,b,c,d) ((unsigned)(((unsigned char)a) << 24) \
 | ((unsigned char)b << 16) \
 | ((unsigned char)c << 8) \
 | ((unsigned char)d))

/*
 * tag_from_string
 * 
 */

static inline unsigned
tag_from_string (char *cs)
{
  unsigned char *s = (unsigned char*) cs;
  return ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]);
}


/*
 * repo_add_susetags
 * Parse susetags file passed in fp, fill solvables into repo
 * 
 * susetags is key,value based
 *  for short values
 *    =key: value
 *  is used
 *  for long (multi-line) values,
 *    +key:
 *    value
 *    value
 *    -key:
 *  is used
 *
 * See http://en.opensuse.org/Standards/YaST2_Repository_Metadata
 * and http://en.opensuse.org/Standards/YaST2_Repository_Metadata/packages
 * and http://en.opensuse.org/Standards/YaST2_Repository_Metadata/pattern
 * 
 * Assumptions:
 *   All keys have 3 characters and end in ':'
 */

void
repo_add_susetags(Repo *repo, FILE *fp, Id vendor, int flags)
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
  Repodata *data = 0;

  data = repo_add_repodata(repo);

  memset(&pd, 0, sizeof(pd));
  line = malloc(1024);
  aline = 1024;

  pd.repo = pd.common.repo = repo;
  pd.data = data;
  pd.common.pool = pool;

  linep = line;
  s = 0;

  /*
   * read complete file
   * 
   * collect values in 'struct parsedata pd'
   * then build .solv (and .attr) file
   */
  
  for (;;)
    {
      unsigned tag;
      if (linep - line + 16 > aline)              /* (re-)alloc buffer */
	{
	  aline = linep - line;
	  line = realloc(line, aline + 512);
	  linep = line + aline;
	  aline += 512;
	}
      if (!fgets(linep, aline - (linep - line), fp)) /* read line */
	break;
      linep += strlen(linep);
      if (linep == line || linep[-1] != '\n')
        continue;
      *--linep = 0;
      
      if (intag)
	{
	  /* check for multi-line value tags (+Key:/-Key:) */
	  
	  int is_end = (linep[-intag - 2] == '-')
	              && (linep[-1] == ':')
	              && !strncmp(linep - 1 - intag, line + 1, intag)
		      && (linep == line + 1 + intag + 1 + 1 + 1 + intag + 1 || linep[-intag - 3] == '\n');
	  if (cummulate && !is_end)
	    {
	      *linep++ = '\n';
	      continue;
	    }
	  if (cummulate && is_end)
	    {
	      linep[-intag - 2] = 0;
	      if (linep[-intag - 3] == '\n')
	        linep[-intag - 3] = 0;
	      linep = line;
	      intag = 0;
	    }
	  if (!cummulate && is_end)
	    {
	      intag = 0;
	      linep = line;
	      continue;
	    }
	  if (!cummulate && !is_end)
	    linep = line + intag + 3;
	}
      else
	linep = line;

      if (!intag && line[0] == '+' && line[1] && line[1] != ':') /* start of +Key:/-Key: tag */
	{
	  char *tagend = strchr(line, ':');
	  if (!tagend)
	    {
	      fprintf(stderr, "bad line: %s\n", line);
	      exit(1);
	    }
	  intag = tagend - (line + 1);
	  cummulate = 0;
	  switch (tag_from_string (line))       /* check if accumulation is needed */
	    {
	      case CTAG('+', 'P', 'r', 'q'):
	      case CTAG('+', 'P', 'r', 'c'):
	      case CTAG('+', 'P', 's', 'g'):
	        if (!pd.kind || !(flags & SUSETAGS_KINDS_SEPARATELY))
		  break;
	      case CTAG('+', 'D', 'e', 's'):
	      case CTAG('+', 'E', 'u', 'l'):
	      case CTAG('+', 'I', 'n', 's'):
	      case CTAG('+', 'D', 'e', 'l'):
	      case CTAG('+', 'A', 'u', 't'):
	        if (line[4] == ':')
	          cummulate = 1;
	    }
	  line[0] = '=';                       /* handle lines between +Key:/-Key: as =Key: */
	  line[intag + 2] = ' ';
	  linep = line + intag + 3;
	  continue;
	}
      if (*line == '#' || !*line)
	continue;
      if (! (line[0] && line[1] && line[2] && line[3] && line[4] == ':'))
        continue;
      tag = tag_from_string (line);


      /*
       * start of (next) package or pattern
       *
       * =Pkg: <name> <version> <release> <architecture>
       * (=Pat: ...)
       */
      
      if ((tag == CTAG('=', 'P', 'k', 'g')
	   || tag == CTAG('=', 'P', 'a', 't')))
	{
	  Id name, evr, arch;
	  /* If we have an old solvable, complete it by filling in some
	     default stuff.  */
	  if (s)
	    {
	      /* A self provide, except for source packages.  This is harmless
	         to do twice (in case we see the same package twice).  */
	      if (s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	        s->provides = repo_addid_dep(repo, s->provides,
					     rel2id(pool, s->name, s->evr,
						    REL_EQ, 1), 0);
	      /* XXX This uses repo_addid_dep internally, so should also be
	         harmless to do twice.  */
	      s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);
	      if (pd.ndirs)
		commit_diskusage (&pd, last_found_pack);
	    }

	  /*
	   * define kind
	   */
	  
	  pd.kind = 0;
	  if (line[3] == 't')
	    pd.kind = "pattern";

	  /*
	   * parse nevra
	   */
	  
          if (split(line + 5, sp, 5) != 4)
	    {
	      fprintf(stderr, "Bad line: %s\n", line);
	      exit(1);
	    }
	  /* Lookup (but don't construct) the name and arch.  */
	  if (pd.kind)
	    name = str2id(pool, join2(pd.kind, ":", sp[0]), 0);
	  else
	    name = str2id(pool, sp[0], 0);
	  arch = str2id(pool, sp[3], 0);
	  evr = makeevr(pool, join2(sp[1], "-", sp[2]));

	  s = 0;

	  /* Now see if we know this solvable already.  If we found neither
	     the name nor the arch at all in this repo
	     there's no chance of finding the exact solvable either.  */
	  if (indesc >= 2 && name && arch)
	    {
	      int n, nn;
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
	    }

	  /* And if we still don't have a solvable, create a new one.  */
	  if (!s)
	    {
	      s = pool_id2solvable(pool, repo_add_solvable(repo));
	      last_found_pack = (s - pool->solvables) - repo->start;
	      if (data)
		repodata_extend(data, s - pool->solvables);
	      if (name)
	        s->name = name;
	      else if (pd.kind)
		s->name = str2id(pool, join2(pd.kind, ":", sp[0]), 1);
	      else
		s->name = str2id(pool, sp[0], 1);
	      s->evr = evr;
	      if (arch)
	        s->arch = arch;
	      else
	        s->arch = str2id(pool, sp[3], 1);
	      s->vendor = vendor;
	    }
	}

      /* If we have no current solvable to add to, ignore all further lines
         for it.  Probably invalid input data in the second set of
	 solvables.  */
      if (indesc >= 2 && !s)
        {
	  fprintf (stderr, "Huh?\n");
          continue;
	}
      switch (tag)
        {
	  case CTAG('=', 'P', 'r', 'v'):                                        /* provides */
	    s->provides = adddep(pool, &pd, s->provides, line, 0, pd.kind);
	    continue;
	  case CTAG('=', 'R', 'e', 'q'):                                        /* requires */
	    s->requires = adddep(pool, &pd, s->requires, line, -SOLVABLE_PREREQMARKER, pd.kind);
	    continue;
          case CTAG('=', 'P', 'r', 'q'):                                        /* pre-requires / packages required */
	    if (pd.kind)
	      {
		if (flags & SUSETAGS_KINDS_SEPARATELY)
		  repodata_set_poolstr(data, last_found_pack, str2id(pool, "solvable:must", 1), line + 6);
		else
		  s->requires = adddep(pool, &pd, s->requires, line, 0, 0);           /* patterns: a required package */
	      }
	    else
	      s->requires = adddep(pool, &pd, s->requires, line, SOLVABLE_PREREQMARKER, 0); /* package: pre-requires */
	    continue;
	  case CTAG('=', 'O', 'b', 's'):                                        /* obsoletes */
	    s->obsoletes = adddep(pool, &pd, s->obsoletes, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'C', 'o', 'n'):                                        /* conflicts */
	    s->conflicts = adddep(pool, &pd, s->conflicts, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'R', 'e', 'c'):                                        /* recommends */
	    s->recommends = adddep(pool, &pd, s->recommends, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'S', 'u', 'p'):                                        /* supplements */
	    s->supplements = adddep(pool, &pd, s->supplements, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'E', 'n', 'h'):                                        /* enhances */
	    s->enhances = adddep(pool, &pd, s->enhances, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'S', 'u', 'g'):                                        /* suggests */
	    s->suggests = adddep(pool, &pd, s->suggests, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'F', 'r', 'e'):                                        /* freshens */
	    s->freshens = adddep(pool, &pd, s->freshens, line, 0, pd.kind);
	    continue;
          case CTAG('=', 'P', 'r', 'c'):                                        /* packages recommended */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      repodata_set_poolstr(data, last_found_pack, str2id(pool, "solvable:should", 1), line + 6);
	    else
	      s->recommends = adddep(pool, &pd, s->recommends, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 's', 'g'):                                        /* packages suggested */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      repodata_set_poolstr(data, last_found_pack, str2id(pool, "solvable:may", 1), line + 6);
	    else
	      s->suggests = adddep(pool, &pd, s->suggests, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 'c', 'n'):                                        /* pattern: package conflicts */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      fprintf (stderr, "Unsupported: pattern -> package conflicts\n");
	    else
	      s->conflicts = adddep(pool, &pd, s->conflicts, line, 0, 0);
	    continue;
	  case CTAG('=', 'P', 'o', 'b'):                                        /* pattern: package obsoletes */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      fprintf (stderr, "Unsupported: pattern -> package obsoletes\n");
	    else
	      s->obsoletes = adddep(pool, &pd, s->obsoletes, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 'f', 'r'):                                        /* pattern: package freshens */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      fprintf (stderr, "Unsupported: pattern -> package freshens\n");
	    else
	      s->freshens = adddep(pool, &pd, s->freshens, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 's', 'p'):                                        /* pattern: package supplements */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      fprintf (stderr, "Unsupported: pattern -> package supplements\n");
	    else
	      s->supplements = adddep(pool, &pd, s->supplements, line, 0, 0);
	    continue;
          case CTAG('=', 'P', 'e', 'n'):                                        /* pattern: package enhances */
	    if (flags & SUSETAGS_KINDS_SEPARATELY)
	      fprintf (stderr, "Unsupported: pattern -> package enhances\n");
	    else
	      s->enhances = adddep(pool, &pd, s->enhances, line, 0, 0);
	    continue;
          case CTAG('=', 'V', 'e', 'r'):                                        /* - version - */
	    last_found_pack = 0;
	    indesc++;
	    continue;

        /* From here it's the attribute tags.  */
          case CTAG('=', 'G', 'r', 'p'):
	    repodata_set_poolstr(data, last_found_pack, SOLVABLE_GROUP, line + 6);
	    continue;
          case CTAG('=', 'L', 'i', 'c'):
	    repodata_set_poolstr(data, last_found_pack, SOLVABLE_LICENSE, line + 6);
	    continue;
          case CTAG('=', 'L', 'o', 'c'):
	    add_location(&pd, line + 6, s, last_found_pack);
	    continue;
          case CTAG('=', 'S', 'r', 'c'):
	    add_source(&pd, line + 6, s, last_found_pack);
	    continue;
          case CTAG('=', 'S', 'i', 'z'):
	    if (split (line + 6, sp, 3) == 2)
	      {
		repodata_set_num(data, last_found_pack, SOLVABLE_DOWNLOADSIZE, (atoi(sp[0]) + 1023) / 1024);
		repodata_set_num(data, last_found_pack, SOLVABLE_INSTALLSIZE, (atoi(sp[1]) + 1023) / 1024);
	      }
	    continue;
          case CTAG('=', 'T', 'i', 'm'):
	    {
	      unsigned int t = atoi (line + 6);
	      if (t)
		repodata_set_num(data, last_found_pack, SOLVABLE_BUILDTIME, t);
	    }
	    continue;
          case CTAG('=', 'K', 'w', 'd'):
	    repodata_set_poolstr(data, last_found_pack, SOLVABLE_KEYWORDS, line + 6);
	    continue;
          case CTAG('=', 'A', 'u', 't'):
	    repodata_set_str(data, last_found_pack, SOLVABLE_AUTHORS, line + 6);
	    continue;
          case CTAG('=', 'S', 'u', 'm'):
	    repodata_set_str(data, last_found_pack, SOLVABLE_SUMMARY, line + 6);
	    continue;
          case CTAG('=', 'D', 'e', 's'):
	    repodata_set_str(data, last_found_pack, SOLVABLE_DESCRIPTION, line + 6);
	    continue;
          case CTAG('=', 'E', 'u', 'l'):
	    repodata_set_str(data, last_found_pack, SOLVABLE_EULA, line + 6);
	    continue;
          case CTAG('=', 'I', 'n', 's'):
	    repodata_set_str(data, last_found_pack, SOLVABLE_MESSAGEINS, line + 6);
	    continue;
          case CTAG('=', 'D', 'e', 'l'):
	    repodata_set_str(data, last_found_pack, SOLVABLE_MESSAGEDEL, line + 6);
	    continue;
          case CTAG('=', 'V', 'i', 's'):
	    {
	      /* Accept numbers and textual bools.  */
	      unsigned k;
	      k = atoi (line + 6);
	      if (k || !strcasecmp (line + 6, "true"))
	        repodata_set_constant(data, last_found_pack, SOLVABLE_ISVISIBLE, 1);
	    }
	    continue;
          case CTAG('=', 'S', 'h', 'r'):
	    if (last_found_pack >= pd.nshare)
	      {
	        if (pd.share_with)
		  {
		    pd.share_with = realloc (pd.share_with, (last_found_pack + 256) * sizeof (*pd.share_with));
		    memset (pd.share_with + pd.nshare, 0, (last_found_pack + 256 - pd.nshare) * sizeof (*pd.share_with));
		  }
		else
		  pd.share_with = calloc (last_found_pack + 256, sizeof (*pd.share_with));
		pd.nshare = last_found_pack + 256;
	      }
	    pd.share_with[last_found_pack] = strdup (line + 6);
	    continue;
	  case CTAG('=', 'D', 'i', 'r'):
	    add_dirline (&pd, line + 6);
	    continue;

	  default:
	    break;
	}

    } /* for(;;) */

  /* add versioned 'self-provides' */
  if (s && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
    s->provides = repo_addid_dep(repo, s->provides, rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);

  if (s)
    s->supplements = repo_fix_legacy(repo, s->provides, s->supplements);

  if (s && pd.ndirs)
    commit_diskusage(&pd, last_found_pack);
    
  /* Shared attributes
   *  (e.g. multiple binaries built from same source)
   */
  if (pd.nshare)
    {
      int i, last_found;
      last_found = 0;
      for (i = 0; i < pd.nshare; i++)
        if (pd.share_with[i])
	  {
	    if (split(pd.share_with[i], sp, 5) != 4)
	      {
	        fprintf(stderr, "Bad =Shr line: %s\n", pd.share_with[i]);
	        exit(1);
	      }

	    Id name = str2id(pool, sp[0], 1);
	    Id evr = makeevr(pool, join2(sp[1], "-", sp[2]));
	    Id arch = str2id(pool, sp[3], 1);
	    unsigned n, nn;
	    Solvable *found = 0;
	    for (n = repo->start, nn = repo->start + last_found;
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
		    last_found = nn - repo->start;
		    break;
		  }
	      }
	    if (n != repo->end)
	      repodata_merge_attrs (data, i, last_found);
	  }
      free (pd.share_with);
    }

  if (data)
    repodata_internalize(data);

  if (pd.common.tmp)
    free(pd.common.tmp);
  free(line);
}
