/* vim: sw=2 et
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <zlib.h>

#include "pool.h"
#include "evr.h"
#include "poolarch.h"
#include "repo_solv.h"
#include "repo_susetags.h"
#include "repo_updateinfoxml.h"
#include "repo_rpmmd.h"
#include "solver.h"
#include "solverdebug.h"

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
showproblems(Solver *solv, Solvable *s, Queue *cand, Queue *badguys)
{
  Pool *pool = solv->pool;
  Queue rids, rinfo;
  Id problem = 0;
  int jj;
  int rerun = 0;

  queue_init(&rids);
  queue_init(&rinfo);
  printf("can't install %s:\n", solvable2str(pool, s));
  while ((problem = solver_next_problem(solv, problem)) != 0)
    {
      solver_findallproblemrules(solv, problem, &rids);
      for (jj = 0; jj < rids.count; jj++)
	{
	  Id probr = rids.elements[jj];
	  int k, l;

	  queue_empty(&rinfo);
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
		  printf("  %s has inferior architecture\n", solvid2str(pool, source));
		  break;
		case SOLVER_PROBLEM_UPDATE_RULE:
		  printf("  update rule for %s\n", solvid2str(pool, source));
		  if (badguys)
		    queue_pushunique(badguys, source);
		  if (!cand)
		    break;
		  /* only drop update problem packages from cand so that we see all problems of this patch */
		  for (l = 0; l < cand->count; l++)
		    if (cand->elements[l] == source || cand->elements[l] == -source)
		      break;
		  if (l == cand->count)
		    break;
		  if (!rerun)
		    {
		      for (l = 0; l < cand->count; l++)
			if (cand->elements[l] < 0)
			  cand->elements[l] = -cand->elements[l];
		      rerun = 1;
		    }
		  for (l = 0; l < cand->count; l++)
		    if (cand->elements[l] == source)
		      {
			cand->elements[l] = -source;
		      }
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
		  printf("  package %s is not installable\n", solvid2str(pool, source));
		  break;
		case SOLVER_PROBLEM_NOTHING_PROVIDES_DEP:
		  printf("  nothing provides %s needed by %s\n", dep2str(pool, dep), solvid2str(pool, source));
		  if (ISRELDEP(dep))
		    {
		      Reldep *rd = GETRELDEP(pool, dep);
		      if (!ISRELDEP(rd->name))
			{
			  Id rp, rpp;
			  FOR_PROVIDES(rp, rpp, rd->name)
			    printf("    (we have %s)\n", solvid2str(pool, rp));
			}
		    }
		  break;
		case SOLVER_PROBLEM_SAME_NAME:
		  printf("  cannot install both %s and %s\n", solvid2str(pool, source), solvid2str(pool, target));
		  break;
		case SOLVER_PROBLEM_PACKAGE_CONFLICT:
		  printf("  package %s conflicts with %s provided by %s\n", solvid2str(pool, source), dep2str(pool, dep), solvid2str(pool, target));
		  break;
		case SOLVER_PROBLEM_PACKAGE_OBSOLETES:
		  printf("  package %s obsoletes %s provided by %s\n", solvid2str(pool, source), dep2str(pool, dep), solvid2str(pool, target));
		  break;
		case SOLVER_PROBLEM_DEP_PROVIDERS_NOT_INSTALLABLE:
		  printf("  package %s requires %s, but none of the providers can be installed\n", solvid2str(pool, source), dep2str(pool, dep));
		  break;
		case SOLVER_PROBLEM_SELF_CONFLICT:
		  printf("  package %s conflicts with %s provided by itself\n", solvid2str(pool, source), dep2str(pool, dep));
		  break;
		}
	    }
	}
    }
  queue_free(&rids);
  queue_free(&rinfo);
}

void
toinst(Solver *solv, Repo *repo, Repo *instrepo)
{
  Pool *pool = solv->pool;
  int k;
  Id p;

  for (k = 0; k < solv->decisionq.count; k++)
    {
      p = solv->decisionq.elements[k];
      if (p < 0 || p == SYSTEMSOLVABLE)
	continue;

     /* printf(" toinstall %s\n", solvid2str(pool, p));*/
      /* oh my! */
      pool->solvables[p].repo = instrepo;
    }
}

void
dump_instrepo(Repo *instrepo, Pool *pool)
{
  Solvable *s;
  Id p;

  printf("instrepo..\n");
  FOR_REPO_SOLVABLES(instrepo, p, s)
    printf("  %s\n", solvable2str(pool, s));
  printf("done.\n");
}

void
frominst(Solver *solv, Repo *repo, Repo *instrepo)
{
  Pool *pool = solv->pool;
  int k;

  for (k = 1; k < pool->nsolvables; k++)
    if (pool->solvables[k].repo == instrepo)
      pool->solvables[k].repo = repo;
}

void
usage(char** argv)
{

  printf("%s: <arch> <patchnameprefix> [repos] [--updaterepos] [repos]...\n"
      "\t repos: repository ending in\n"
      "\t\tpackages, packages.gz, primary.xml.gz, updateinfo.xml.gz or .solv\n",
      argv[0]);

  exit(1);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  char *arch, *mypatch;
  const char *pname;
  int l;
  FILE *fp;
  int i, j;
  Queue job;
  Queue cand;
  Queue badguys;
  Id pid, p, pp;
  Id con, *conp;
  Solver *solv;
  Repo *repo, *instrepo;
  int status = 0;
  int tests = 0;
  int updatestart = 0;

  if (argc <= 3)
    usage(argv);

  arch = argv[1];
  pool = pool_create();
  pool_setarch(pool, arch);
  static const char* langs[] = {"en"};
  pool_set_languages(pool, langs, 1);

#if 0
  pool_setdebuglevel(pool, 2);
#endif

  mypatch = argv[2];

  repo = repo_create(pool, 0);
  instrepo = repo_create(pool, 0);
  for (i = 3; i < argc; i++)
    {
      if (!strcmp(argv[i], "--updaterepos"))
	{
	  updatestart = pool->nsolvables;
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
      else if (l >= 17 && !strcmp(argv[i] + l - 17, "updateinfo.xml.gz"))
	{
          repo_add_updateinfoxml(repo, fp, 0);
	}
      else if (repo_add_solv(repo, fp))
        {
          fprintf(stderr, "could not add repo %s\n", argv[i]);
          exit(1);
        }
      if (fp != stdin)
        fclose(fp);
    }

  pool_addfileprovides(pool);

  /* bad hack ahead: clone repo */
  instrepo->idarraydata = repo->idarraydata;
  instrepo->idarraysize = repo->idarraysize;
  instrepo->start = repo->start;
  instrepo->end = repo->end;
  instrepo->nsolvables = repo->nsolvables;	/* sic! */
  instrepo->lastoff = repo->lastoff;	/* sic! */
  pool_set_installed(pool, instrepo);
  pool_createwhatprovides(pool);

  queue_init(&job);
  queue_init(&cand);
  queue_init(&badguys);

  for (pid = 1; pid < pool->nsolvables; pid++)
    {
      Solvable *s = pool->solvables + pid;
      if (!s->repo)
        continue;
      if (!pool_installable(pool, s))
        continue;
      pname = id2str(pool, s->name);
      if (strncmp(pname, "patch:", 6) != 0)
	continue;

      if (*mypatch)
	{
	  if (strncmp(mypatch, pname + 6, strlen(pname + 6)) != 0)
	    continue;
	  if (strcmp(mypatch, pname + 6) != 0)
	    {
	      l = strlen(pname + 6);
	      if (mypatch[l] != '-')
		continue;
	      if (strcmp(mypatch + l + 1, id2str(pool, s->evr)) != 0)
		continue;
	    }
	}
      else
	{
	  FOR_PROVIDES(p, pp, s->name)
	    {
	      Solvable *s2 = pool->solvables + p;
	      if (evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE) < 0)
		break;
	    }
	  if (p) {
            /* printf("found a newer one for %s\n", pname+6); */
	    continue;	/* found a newer one */
          }
	}
      tests++;
      if (!s->conflicts)
	continue;

#if 0
      printf("testing patch %s-%s\n", pname + 6, id2str(pool, s->evr));
#endif

      if (1) {

      /* Test 1: are all old patches included */
      FOR_PROVIDES(p, pp, s->name)
        {
	  Solvable *s2 = pool->solvables + p;
	  Id con2, *conp2;
	  int shown = 0;

	  if (evrcmp(pool, s->evr, s2->evr, EVRCMP_COMPARE) <= 0)
	    continue;
	  if (!s2->conflicts)
	    continue;
	  conp2 = s2->repo->idarraydata + s2->conflicts;
          while ((con2 = *conp2++) != 0)
	    {
	      Reldep *rd2;
	      if (!ISRELDEP(con2))
		continue;
	      rd2 = GETRELDEP(pool, con2);
	      conp = s->repo->idarraydata + s->conflicts;
	      while ((con = *conp++) != 0)
		{
		  Reldep *rd;
		  if (!ISRELDEP(con))
		    continue;
		  rd = GETRELDEP(pool, con);
		  if (rd->name == rd2->name)
		    break;
		}
	      if (!con)
		{
		  if (!shown++)
		    printf("%s:\n", solvable2str(pool, s));
		  printf("  %s contained %s\n", solvable2str(pool, s2), dep2str(pool, rd2->name));
		}
	    }
	}
      }

      if (1) {

      /* Test 2: are the packages installable */
      conp = s->repo->idarraydata + s->conflicts;
      while ((con = *conp++) != 0)
	{
	  FOR_PROVIDES(p, pp, con)
	    {
	      queue_empty(&job);
	      queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE|SOLVER_WEAK);
	      queue_push(&job, p);

	      /* also set up some minimal system */
	      queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE_PROVIDES|SOLVER_WEAK);
	      queue_push(&job, str2id(pool, "rpm", 1));
	      queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE_PROVIDES|SOLVER_WEAK);
	      queue_push(&job, str2id(pool, "aaa_base", 1));

	      solv = solver_create(pool);
	      solv->dontinstallrecommended = 0;
	      solver_solve(solv, &job);
	      if (solv->problems.count)
		{
		  status = 1;
                  printf("error installing original package\n");
		  showproblems(solv, s, 0, 0);
		}
	      toinst(solv, repo, instrepo);
	      solver_free(solv);

#if 0
              dump_instrepo(instrepo, pool);

#endif
#if 1
	      queue_empty(&job);
	      for (i = 1; i < updatestart; i++)
		{
		  if (pool->solvables[i].repo != repo || i == pid)
		    continue;
		  queue_push(&job, SOLVER_ERASE|SOLVER_SOLVABLE);
		  queue_push(&job, i);
		}
	      queue_push(&job, SOLVER_INSTALL_SOLVABLE);
	      queue_push(&job, pid);
	      solv = solver_create(pool);
	      /*solv->dontinstallrecommended = 1;*/
	      solver_solve(solv, &job);
	      if (solv->problems.count)
		{
		  status = 1;
		  showproblems(solv, s, 0, 0);
		}
	      frominst(solv, repo, instrepo);
	      solver_free(solv);
#endif
	    }
	}
      }

      if (1) {

      /* Test 3: can we upgrade all packages? */
      queue_empty(&cand);
      queue_empty(&badguys);
      for (p = 1; p < pool->nsolvables; p++)
	{
	  Solvable *s = pool->solvables + p;
          if (!s->repo)
	    continue;
          if (strchr(id2str(pool, s->name), ':'))
	    continue;	/* only packages, please */
	  if (!pool_installable(pool, s))
	    continue;
	  queue_push(&cand, p);
	}
      while (cand.count)
	{
	  solv = solver_create(pool);
	  queue_empty(&job);
	  for (i = 0; i < badguys.count; i++)
	    {
	      queue_push(&job, SOLVER_ERASE|SOLVER_SOLVABLE|SOLVER_WEAK);
	      queue_push(&job, badguys.elements[i]);
	    }
	  conp = s->repo->idarraydata + s->conflicts;
          while ((con = *conp++) != 0)
	    {
	      queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE_PROVIDES|SOLVER_WEAK);
	      queue_push(&job, con);
	    }
	  for (i = 0; i < cand.count; i++)
	    {
	      p = cand.elements[i];
	      queue_push(&job, SOLVER_INSTALL|SOLVER_SOLVABLE|SOLVER_WEAK);
	      queue_push(&job, p);
	    }
	  solver_solve(solv, &job);
#if 0
	  solver_printdecisions(solv);
#endif
	  /* put packages into installed repo and prune them from cand */
          toinst(solv, repo, instrepo);
	  for (i = 0; i < cand.count; i++)
	    {
	      p = cand.elements[i];
	      if (p > 0 && solv->decisionmap[p] > 0)
		cand.elements[i] = -p;	/* drop candidate */
	    }
	  solver_free(solv);

	  /* now the interesting part: test patch */
	  queue_empty(&job);
#if 0
	  for (i = 1; i < updatestart; i++)
	    {
	      if (pool->solvables[i].repo != repo || i == pid)
		continue;
	      queue_push(&job, SOLVER_ERASE|SOLVER_SOLVABLE);
	      queue_push(&job, i);
	    }
#endif
	  queue_push(&job, SOLVER_INSTALL_SOLVABLE);
	  queue_push(&job, pid);
	  solv = solver_create(pool);
	  solv->dontinstallrecommended = 1;
	  solver_solve(solv, &job);

	  if (solv->problems.count)
	    {
	      status = 1;
	      showproblems(solv, s, &cand, &badguys);
	    }
          frominst(solv, repo, instrepo);
	  solver_free(solv);
	  /* now drop all negative elements from cand */
	  for (i = j = 0; i < cand.count; i++)
	    {
	      if (cand.elements[i] < 0)
		continue;
	      cand.elements[j++] = cand.elements[i];
	    }
	  if (i == j)
	    break;	/* no progress */
	  cand.count = j;
	}
      }
    }

  exit(status);
}
