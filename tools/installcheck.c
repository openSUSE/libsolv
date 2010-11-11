/* vim: sw=2 et cino=>4,n-2,{1s
 */

/*
 * Copyright (c) 2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */


#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <zlib.h>

#include "pool.h"
#include "poolarch.h"
#include "repo_solv.h"
#ifndef DEBIAN
#include "repo_susetags.h"
#include "repo_rpmmd.h"
#else
#include "repo_deb.h"
#endif
#include "solver.h"

static ssize_t
cookie_gzread(void *cookie, char *buf, size_t nbytes)
{
  return gzread((gzFile *)cookie, buf, nbytes);
}

static int
cookie_gzclose(void *cookie)
{
  return gzclose((gzFile *)cookie);
}

FILE *
myfopen(const char *fn)
{
  cookie_io_functions_t cio;
  char *suf;
  gzFile *gzf;

  if (!fn)
    return 0;
  suf = strrchr(fn, '.');
  if (!suf || strcmp(suf, ".gz") != 0)
    return fopen(fn, "r");
  gzf = gzopen(fn, "r");
  if (!gzf)
    return 0;
  memset(&cio, 0, sizeof(cio));
  cio.read = cookie_gzread;
  cio.close = cookie_gzclose;
  return  fopencookie(gzf, "r", cio);
}

void
usage(char** argv)
{
  printf("Usage:\n%s: <arch> [options..] repo [--nocheck repo]...\n"
         "\t--exclude <pattern>\twhitespace-separated list of (sub-)"
         "packagenames to ignore\n"
         "\t--withsrc\t\tAlso check dependencies of src.rpm\n\n"
         , argv[0]);
  exit(1);
}


int
main(int argc, char **argv)
{
  Pool *pool;
  Solver *solv;
  Queue job;
  Queue rids;
  Queue cand;
  Queue archlocks;
  char *arch, *exclude_pat;
  int i, j;
  Id p;
  Id rpmid, rpmarch, rpmrel, archlock;
  int status = 0;
  int nocheck = 0;
  int withsrc = 0;

  exclude_pat = 0;
  archlock = 0;
  if (argc < 3)
    usage(argv);

  arch = argv[1];
  pool = pool_create();
  pool_setarch(pool, arch);
  for (i = 2; i < argc; i++)
    {
      FILE *fp;
      int l;

      if (!strcmp(argv[i], "--withsrc"))
	{
	  withsrc++;
	  continue;
	}
      if (!strcmp(argv[i], "--nocheck"))
	{
	  if (!nocheck)
	    nocheck = pool->nsolvables;
	  continue;
	}
      if (!strcmp(argv[i], "--exclude"))
        {
          if (i + 1 >= argc)
            {
              printf("--exclude needs a whitespace separated list of substrings as parameter\n");
              exit(1);
            }
          exclude_pat = argv[i + 1];
          ++i;
          continue;
        }
      l = strlen(argv[i]);
      if (!strcmp(argv[i], "-"))
	fp = stdin;
      else if ((fp = myfopen(argv[i])) == 0)
	{
	  perror(argv[i]);
	  exit(1);
	}
      Repo *repo = repo_create(pool, argv[i]);
#ifndef DEBIAN
      if (l >= 8 && !strcmp(argv[i] + l - 8, "packages"))
	{
	  repo_add_susetags(repo, fp, 0, 0, 0);
	}
      else if (l >= 11 && !strcmp(argv[i] + l - 11, "packages.gz"))
	{
	  repo_add_susetags(repo, fp, 0, 0, 0);
	}
      else if (l >= 14 && !strcmp(argv[i] + l - 14, "primary.xml.gz"))
	{
	  repo_add_rpmmd(repo, fp, 0, 0);
	}
#else
      if (l >= 8 && !strcmp(argv[i] + l - 8, "Packages"))
	{
	  repo_add_debpackages(repo, fp, 0);
	}
      else if (l >= 11 && !strcmp(argv[i] + l - 11, "Packages.gz"))
	{
	  repo_add_debpackages(repo, fp, 0);
	}
#endif
      else if (repo_add_solv(repo, fp))
	{
	  fprintf(stderr, "could not add repo %s\n", argv[i]);
	  exit(1);
	}
      if (fp != stdin)
        fclose(fp);
    }
  pool_addfileprovides(pool);
  pool_createwhatprovides(pool);
  rpmid = str2id(pool, "rpm", 0);
  rpmarch = str2id(pool, arch, 0);
  rpmrel = 0;
#ifndef DEBIAN
  if (rpmid && rpmarch)
    {
      for (p = 1; p < pool->nsolvables; p++)
	{
	  Solvable *s = pool->solvables + p;
	  if (s->name == rpmid && s->arch == rpmarch)
	    break;
	}
      if (p < pool->nsolvables)
        rpmrel = rel2id(pool, rpmid, rpmarch, REL_ARCH, 1);
    }
#endif
  
  queue_init(&job);
  queue_init(&rids);
  queue_init(&cand);
  queue_init(&archlocks);
  for (p = 1; p < pool->nsolvables; p++)
    {
      Solvable *s = pool->solvables + p;
      if (!s->repo)
	continue;
      if (withsrc && (s->arch == ARCH_SRC || s->arch == ARCH_NOSRC))
	{
	  queue_push(&cand, p);
	  continue;
	}
      if (!pool_installable(pool, s))
	continue;
      if (rpmrel && s->arch != rpmarch)
	{
	  Id rp, rpp;
	  FOR_PROVIDES(rp, rpp, s->name)
	    {
	      if (pool->solvables[rp].name != s->name)
		continue;
	      if (pool->solvables[rp].arch == rpmarch)
		break;
	    }
	  if (rp)
	    {
	      queue_push(&archlocks, p);
	      continue;
	    }
	}
      queue_push(&cand, p);
    }
  
  if (archlocks.count)
    {
      archlock = pool_queuetowhatprovides(pool, &archlocks);
    }
  /* prune cand by doing weak installs */
  while (cand.count)
    {
      solv = solver_create(pool);
      queue_empty(&job);
      for (i = 0; i < cand.count; i++)
	{
	  p = cand.elements[i];
	  queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE|SOLVER_WEAK);
	  queue_push(&job, p);
	}
      if (rpmrel)
	{
	  queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE_NAME);
	  queue_push(&job, rpmrel);
	}
      if (archlock)
	{
	  queue_push(&job, SOLVER_LOCK|SOLVER_SOLVABLE_ONE_OF);
	  queue_push(&job, archlock);
	}
      solv->dontinstallrecommended = 1;
      solver_solve(solv, &job);
      /* prune... */
      for (i = j = 0; i < cand.count; i++)
	{
	  p = cand.elements[i];
	  if (solv->decisionmap[p] <= 0)
	    {
	      cand.elements[j++] = p;
	      continue;
	    }
#if 0
	  Solvable *s = pool->solvables + p;
	  if (!strcmp(id2str(pool, s->name), "libusb-compat-devel"))
	    {
	      cand.elements[j++] = p;
	      continue;
	    }
#endif
	}
      cand.count = j;
      if (i == j)
	break;
    }

  /* now check every candidate */
  for (i = 0; i < cand.count; i++)
    {
      Solvable *s;

      p = cand.elements[i];
      if (nocheck && p >= nocheck)
	continue;
      if (exclude_pat)
        {
          char *ptr, *save = 0, *pattern;
          int match = 0;
          pattern = strdup(exclude_pat);

          for (ptr = strtok_r(pattern, " ", &save);
              ptr;
              ptr = strtok_r(NULL, " ", &save))
            {
              if (*ptr && strstr(solvid2str(pool, p), ptr))
                {
                  match = 1;
                  break;
                }
            }
          free(pattern);
          if (match)
            continue;
        }
      s = pool->solvables + p;
      solv = solver_create(pool);
      queue_empty(&job);
      queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE);
      queue_push(&job, p);
      if (rpmrel)
	{
	  queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE_NAME);
	  queue_push(&job, rpmrel);
	}
      if (archlock)
	{
	  queue_push(&job, SOLVER_LOCK|SOLVER_SOLVABLE_ONE_OF);
	  queue_push(&job, archlock);
	}
      solv->dontinstallrecommended = 1;
      solver_solve(solv, &job);
      if (solv->problems.count)
	{
	  Id problem = 0;
	  Solvable *s2;

	  status = 1;
	  printf("can't install %s:\n", solvable2str(pool, s));
	  while ((problem = solver_next_problem(solv, problem)) != 0)
	    {
	      solver_findallproblemrules(solv, problem, &rids);
	      for (j = 0; j < rids.count; j++)
		{
		  Id probr = rids.elements[j];
		  int k;
		  Queue rinfo;
		  queue_init(&rinfo);

		  solver_allruleinfos(solv, probr, &rinfo);
		  for (k = 0; k < rinfo.count; k += 4)
		    {
		      Id dep, source, target;
		      source = rinfo.elements[k + 1];
		      target = rinfo.elements[k + 2];
		      dep = rinfo.elements[k + 3];
		      switch (rinfo.elements[k])
			{
			case SOLVER_PROBLEM_DISTUPGRADE_RULE:
			  break;
			case SOLVER_PROBLEM_INFARCH_RULE:
			  s = pool_id2solvable(pool, source);
			  printf("  %s has inferior architecture\n", solvable2str(pool, s));
			  break;
			case SOLVER_PROBLEM_UPDATE_RULE:
			  break;
			case SOLVER_PROBLEM_JOB_RULE:
			  break;
			case SOLVER_PROBLEM_RPM_RULE:
			  printf("  some dependency problem\n");
			  break;
			case SOLVER_PROBLEM_JOB_NOTHING_PROVIDES_DEP:
			  printf("  nothing provides requested %s\n", dep2str(pool, dep));
			  break;
			case SOLVER_PROBLEM_NOT_INSTALLABLE:
			  s = pool_id2solvable(pool, source);
			  printf("  package %s is not installable\n", solvable2str(pool, s));
			  break;
			case SOLVER_PROBLEM_NOTHING_PROVIDES_DEP:
			  s = pool_id2solvable(pool, source);
			  printf("  nothing provides %s needed by %s\n", dep2str(pool, dep), solvable2str(pool, s));
			  if (ISRELDEP(dep))
			    {
			      Reldep *rd = GETRELDEP(pool, dep);
			      if (!ISRELDEP(rd->name))
				{
				  Id rp, rpp;
				  FOR_PROVIDES(rp, rpp, rd->name)
				    printf("    (we have %s)\n", solvable2str(pool, pool->solvables + rp));
				}
			    }
			  break;
			case SOLVER_PROBLEM_SAME_NAME:
			  s = pool_id2solvable(pool, source);
			  s2 = pool_id2solvable(pool, target);
			  printf("  cannot install both %s and %s\n", solvable2str(pool, s), solvable2str(pool, s2));
			  break;
			case SOLVER_PROBLEM_PACKAGE_CONFLICT:
			  s = pool_id2solvable(pool, source);
			  s2 = pool_id2solvable(pool, target);
			  printf("  package %s conflicts with %s provided by %s\n", solvable2str(pool, s), dep2str(pool, dep), solvable2str(pool, s2));
			  break;
			case SOLVER_PROBLEM_PACKAGE_OBSOLETES:
			  s = pool_id2solvable(pool, source);
			  s2 = pool_id2solvable(pool, target);
			  printf("  package %s obsoletes %s provided by %s\n", solvable2str(pool, s), dep2str(pool, dep), solvable2str(pool, s2));
			  break;
			case SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE:
			  s = pool_id2solvable(pool, source);
			  printf("  package %s requires %s, but none of the providers can be installed\n", solvable2str(pool, s), dep2str(pool, dep));
			  break;
			case SOLVER_PROBLEM_SELF_CONFLICT:
			  s = pool_id2solvable(pool, source);
			  printf("  package %s conflicts with %s provided by itself\n", solvable2str(pool, s), dep2str(pool, dep));
			  break;
			}
		    }
		}
	    }
	}
#if 0
      else
	{
	  if (!strcmp(id2str(pool, s->name), "libusb-compat-devel"))
	    {
	      solver_printdecisions(solv);
	    }
	}
#endif
      solver_free(solv);
    }
  exit(status);
}
