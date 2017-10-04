/*
 * Copyright (c) 2009-2015, SUSE LLC.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* solv, a little software installer demoing the sat solver library */

/* things it does:
 * - understands globs for package names / dependencies
 * - understands .arch suffix
 * - installation of commandline packages
 * - repository data caching
 * - on demand loading of secondary repository data
 * - gpg and checksum verification
 * - file conflicts
 * - deltarpm support
 * - fastestmirror implementation
 *
 * things available in the library but missing from solv:
 * - vendor policy loading
 * - multi version handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "pool.h"
#include "poolarch.h"
#include "evr.h"
#include "selection.h"
#include "repo.h"
#include "solver.h"
#include "solverdebug.h"
#include "transaction.h"
#include "testcase.h"
#ifdef SUSE
#include "repo_autopattern.h"
#endif

#include "repoinfo.h"
#include "repoinfo_cache.h"
#include "repoinfo_download.h"

#if defined(ENABLE_RPMDB)
#include "fileprovides.h"
#include "fileconflicts.h"
#include "deltarpm.h"
#endif
#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
#include "patchjobs.h"
#endif

void
setarch(Pool *pool)
{
  struct utsname un;
  if (uname(&un))
    {
      perror("uname");
      exit(1);
    }
  pool_setarch(pool, un.machine);
}


int
yesno(const char *str, int other)
{
  char inbuf[128], *ip;

  for (;;)
    {
      printf("%s", str);
      fflush(stdout);
      *inbuf = 0;
      if (!(ip = fgets(inbuf, sizeof(inbuf), stdin)))
	{
	  printf("Abort.\n");
	  exit(1);
	}
      while (*ip == ' ' || *ip == '\t')
	ip++;
      if (*ip == 'q')
	{
	  printf("Abort.\n");
	  exit(1);
	}
      if (*ip == 'y' || *ip == 'n' || *ip == other)
	return *ip == 'n' ? 0 : *ip;
    }
}

#ifdef SUSE
static Id
nscallback(Pool *pool, void *data, Id name, Id evr)
{
#if 0
  if (name == NAMESPACE_LANGUAGE)
    {
      if (!strcmp(pool_id2str(pool, evr), "ja"))
	return 1;
      if (!strcmp(pool_id2str(pool, evr), "de"))
	return 1;
      if (!strcmp(pool_id2str(pool, evr), "en"))
	return 1;
      if (!strcmp(pool_id2str(pool, evr), "en_US"))
	return 1;
    }
#endif
  return 0;
}
#endif

#ifdef SUSE
static void
showdiskusagechanges(Transaction *trans)
{
  DUChanges duc[4];
  int i;

  /* XXX: use mountpoints here */
  memset(duc, 0, sizeof(duc));
  duc[0].path = "/";
  duc[1].path = "/usr/share/man";
  duc[2].path = "/sbin";
  duc[3].path = "/etc";
  transaction_calc_duchanges(trans, duc, 4);
  for (i = 0; i < 4; i++)
    printf("duchanges %s: %d K  %d inodes\n", duc[i].path, duc[i].kbytes, duc[i].files);
}
#endif

static Id
find_repo(const char *name, Pool *pool, struct repoinfo *repoinfos, int nrepoinfos)
{
  const char *rp;
  int i;

  for (rp = name; *rp; rp++)
    if (*rp <= '0' || *rp >= '9')
      break;
  if (!*rp)
    {
      /* repo specified by number */
      int rnum = atoi(name);
      for (i = 0; i < nrepoinfos; i++)
	{
	  struct repoinfo *cinfo = repoinfos + i;
	  if (!cinfo->enabled || !cinfo->repo)
	    continue;
	  if (--rnum == 0)
	    return cinfo->repo->repoid;
	}
    }
  else
    {
      /* repo specified by alias */
      Repo *repo;
      FOR_REPOS(i, repo)
	{
	  if (!strcasecmp(name, repo->name))
	    return repo->repoid;
	}
    }
  return 0;
}


#define MODE_LIST        0
#define MODE_INSTALL     1
#define MODE_ERASE       2
#define MODE_UPDATE      3
#define MODE_DISTUPGRADE 4
#define MODE_VERIFY      5
#define MODE_PATCH       6
#define MODE_INFO        7
#define MODE_REPOLIST    8
#define MODE_SEARCH	 9

void
usage(int r)
{
  fprintf(stderr, "Usage: solv COMMAND <select>\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "    dist-upgrade: replace installed packages with\n");
  fprintf(stderr, "                  versions from the repositories\n");
  fprintf(stderr, "    erase:        erase installed packages\n");
  fprintf(stderr, "    info:         display package information\n");
  fprintf(stderr, "    install:      install packages\n");
  fprintf(stderr, "    list:         list packages\n");
  fprintf(stderr, "    repos:        list enabled repositories\n");
  fprintf(stderr, "    search:       search name/summary/description\n");
  fprintf(stderr, "    update:       update installed packages\n");
  fprintf(stderr, "    verify:       check dependencies of installed packages\n");
#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
  fprintf(stderr, "    patch:        install newest maintenance updates\n");
#endif
  fprintf(stderr, "\n");
  exit(r);
}

int
main(int argc, char **argv)
{
  Pool *pool;
  Repo *commandlinerepo = 0;
  Id *commandlinepkgs = 0;
  Id p;
  struct repoinfo *repoinfos, installedrepoinfo;
  int nrepoinfos = 0;
  int mainmode = 0, mode = 0;
  int i, newpkgs;
  Queue job, checkq;
  Solver *solv = 0;
  Transaction *trans;
  FILE **newpkgsfps;
  Queue repofilter;
  Queue kindfilter;
  Queue archfilter;
  int archfilter_src = 0;
  int cleandeps = 0;
  int forcebest = 0;
  char *rootdir = 0;
  char *keyname = 0;
  int keyname_depstr = 0;
  int debuglevel = 0;
  int answer, acnt = 0;
  char *testcase = 0;

  argc--;
  argv++;
  while (argc && !strcmp(argv[0], "-d"))
    {
      debuglevel++;
      argc--;
      argv++;
    }
  if (!argv[0])
    usage(1);
  if (!strcmp(argv[0], "install") || !strcmp(argv[0], "in"))
    {
      mainmode = MODE_INSTALL;
      mode = SOLVER_INSTALL;
    }
#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
  else if (!strcmp(argv[0], "patch"))
    {
      mainmode = MODE_PATCH;
      mode = SOLVER_INSTALL;
    }
#endif
  else if (!strcmp(argv[0], "erase") || !strcmp(argv[0], "rm"))
    {
      mainmode = MODE_ERASE;
      mode = SOLVER_ERASE;
    }
  else if (!strcmp(argv[0], "list") || !strcmp(argv[0], "ls"))
    {
      mainmode = MODE_LIST;
      mode = 0;
    }
  else if (!strcmp(argv[0], "info"))
    {
      mainmode = MODE_INFO;
      mode = 0;
    }
  else if (!strcmp(argv[0], "search") || !strcmp(argv[0], "se"))
    {
      mainmode = MODE_SEARCH;
      mode = 0;
    }
  else if (!strcmp(argv[0], "verify"))
    {
      mainmode = MODE_VERIFY;
      mode = SOLVER_VERIFY;
    }
  else if (!strcmp(argv[0], "update") || !strcmp(argv[0], "up"))
    {
      mainmode = MODE_UPDATE;
      mode = SOLVER_UPDATE;
    }
  else if (!strcmp(argv[0], "dist-upgrade") || !strcmp(argv[0], "dup"))
    {
      mainmode = MODE_DISTUPGRADE;
      mode = SOLVER_DISTUPGRADE;
    }
  else if (!strcmp(argv[0], "repos") || !strcmp(argv[0], "repolist") || !strcmp(argv[0], "lr"))
    {
      mainmode = MODE_REPOLIST;
      mode = 0;
    }
  else
    usage(1);

  for (;;)
    {
      if (argc > 2 && !strcmp(argv[1], "--root"))
	{
	  rootdir = argv[2];
	  argc -= 2;
	  argv += 2;
	}
      else if (argc > 1 && !strcmp(argv[1], "--clean"))
	{
	  cleandeps = 1;
	  argc--;
	  argv++;
	}
      else if (argc > 1 && !strcmp(argv[1], "--best"))
	{
	  forcebest = 1;
	  argc--;
	  argv++;
	}
      else if (argc > 1 && !strcmp(argv[1], "--depstr"))
	{
	  keyname_depstr = 1;
	  argc--;
	  argv++;
	}
      else if (argc > 2 && !strcmp(argv[1], "--keyname"))
	{
	  keyname = argv[2];
	  argc -= 2;
	  argv += 2;
	}
      else if (argc > 2 && !strcmp(argv[1], "--testcase"))
	{
	  testcase = argv[2];
	  argc -= 2;
	  argv += 2;
	}
      else
	break;
    }

  set_userhome();
  pool = pool_create();
  pool_set_rootdir(pool, rootdir);

#if 0
  {
    const char *langs[] = {"de_DE", "de", "en"};
    pool_set_languages(pool, langs, sizeof(langs)/sizeof(*langs));
  }
#endif

  pool_setloadcallback(pool, load_stub, 0);
#ifdef SUSE
  pool->nscallback = nscallback;
#endif
  if (debuglevel)
    pool_setdebuglevel(pool, debuglevel);
  setarch(pool);
  pool_set_flag(pool, POOL_FLAG_ADDFILEPROVIDESFILTERED, 1);
  repoinfos = read_repoinfos(pool, &nrepoinfos);
  sort_repoinfos(repoinfos, nrepoinfos);

  if (mainmode == MODE_REPOLIST)
    {
      int j = 1;
      for (i = 0; i < nrepoinfos; i++)
	{
	  struct repoinfo *cinfo = repoinfos + i;
	  if (!cinfo->enabled)
	    continue;
	  printf("%d: %-20s %s (prio %d)\n", j++, cinfo->alias, cinfo->name, cinfo->priority);
	}
      exit(0);
    }
  memset(&installedrepoinfo, 0, sizeof(installedrepoinfo));
  if (!read_installed_repo(&installedrepoinfo, pool))
    exit(1);
  read_repos(pool, repoinfos, nrepoinfos);

  /* setup filters */
  queue_init(&repofilter);
  queue_init(&kindfilter);
  queue_init(&archfilter);
  while (argc > 1)
    {
      if (!strcmp(argv[1], "-i"))
	{
	  queue_push2(&repofilter, SOLVER_SOLVABLE_REPO | SOLVER_SETREPO, pool->installed->repoid);
	  argc--;
	  argv++;
	}
      else if (argc > 2 && (!strcmp(argv[1], "-r") || !strcmp(argv[1], "--repo")))
	{
	  Id repoid = find_repo(argv[2], pool, repoinfos, nrepoinfos);
	  if (!repoid)
	    {
	      fprintf(stderr, "%s: no such repo\n", argv[2]);
	      exit(1);
	    }
	  /* SETVENDOR is actually wrong but useful */
	  queue_push2(&repofilter, SOLVER_SOLVABLE_REPO | SOLVER_SETREPO | SOLVER_SETVENDOR, repoid);
	  argc -= 2;
	  argv += 2;
	}
      else if (argc > 2 && !strcmp(argv[1], "--arch"))
	{
	  if (!strcmp(argv[2], "src") || !strcmp(argv[2], "nosrc"))
	    archfilter_src = 1;
	  queue_push2(&archfilter, SOLVER_SOLVABLE_PROVIDES, pool_rel2id(pool, 0, pool_str2id(pool, argv[2], 1), REL_ARCH, 1));
	  argc -= 2;
	  argv += 2;
	}
      else if (argc > 2 && (!strcmp(argv[1], "-t") || !strcmp(argv[1], "--type")))
	{
	  const char *kind = argv[2];
	  if (!strcmp(kind, "srcpackage"))
	    {
	      /* hey! should use --arch! */
	      queue_push2(&archfilter, SOLVER_SOLVABLE_PROVIDES, pool_rel2id(pool, 0, ARCH_SRC, REL_ARCH, 1));
	      archfilter_src = 1;
	      argc -= 2;
	      argv += 2;
	      continue;
	    }
	  if (!strcmp(kind, "package"))
	    kind = "";
	  if (!strcmp(kind, "all"))
	    queue_push2(&kindfilter, SOLVER_SOLVABLE_ALL, 0);
	  else
	    queue_push2(&kindfilter, SOLVER_SOLVABLE_PROVIDES, pool_rel2id(pool, 0, pool_str2id(pool, kind, 1), REL_KIND, 1));
	  argc -= 2;
	  argv += 2;
	}
      else
	break;
    }

  if (mainmode == MODE_SEARCH)
    {
      Queue sel, q;
      Dataiterator di;
      if (argc != 2)
	usage(1);
      pool_createwhatprovides(pool);
      queue_init(&sel);
      dataiterator_init(&di, pool, 0, 0, 0, argv[1], SEARCH_SUBSTRING|SEARCH_NOCASE);
      dataiterator_set_keyname(&di, SOLVABLE_NAME);
      dataiterator_set_search(&di, 0, 0);
      while (dataiterator_step(&di))
	queue_push2(&sel, SOLVER_SOLVABLE, di.solvid);
      dataiterator_set_keyname(&di, SOLVABLE_SUMMARY);
      dataiterator_set_search(&di, 0, 0);
      while (dataiterator_step(&di))
	queue_push2(&sel, SOLVER_SOLVABLE, di.solvid);
      dataiterator_set_keyname(&di, SOLVABLE_DESCRIPTION);
      dataiterator_set_search(&di, 0, 0);
      while (dataiterator_step(&di))
	queue_push2(&sel, SOLVER_SOLVABLE, di.solvid);
      dataiterator_free(&di);
      if (repofilter.count)
	selection_filter(pool, &sel, &repofilter);
	
      queue_init(&q);
      selection_solvables(pool, &sel, &q);
      queue_free(&sel);
      for (i = 0; i < q.count; i++)
	{
	  Solvable *s = pool_id2solvable(pool, q.elements[i]);
	  printf("  - %s [%s]: %s\n", pool_solvable2str(pool, s), s->repo->name, solvable_lookup_str(s, SOLVABLE_SUMMARY));
	}
      queue_free(&q);
      exit(0);
    }

  /* process command line packages */
  if (mainmode == MODE_LIST || mainmode == MODE_INFO || mainmode == MODE_INSTALL)
    {
      for (i = 1; i < argc; i++)
	{
	  if (!is_cmdline_package((const char *)argv[i]))
	    continue;
	  if (access(argv[i], R_OK))
	    {
	      perror(argv[i]);
	      exit(1);
	    }
	  if (!commandlinepkgs)
	    commandlinepkgs = solv_calloc(argc, sizeof(Id));
	  if (!commandlinerepo)
	    commandlinerepo = repo_create(pool, "@commandline");
	  p = add_cmdline_package(commandlinerepo, (const char *)argv[i]);
	  if (!p)
	    {
	      fprintf(stderr, "could not add '%s'\n", argv[i]);
	      exit(1);
	    }
	  commandlinepkgs[i] = p;
	}
      if (commandlinerepo)
	{
	  repo_internalize(commandlinerepo);
#ifdef SUSE
	  repo_add_autopattern(commandlinerepo, 0);
#endif
	}
    }

#if defined(ENABLE_RPMDB)
  if (pool->disttype == DISTTYPE_RPM)
    addfileprovides(pool);
#endif
  pool_createwhatprovides(pool);

  if (keyname)
    keyname = solv_dupjoin("solvable:", keyname, 0);
  queue_init(&job);
  for (i = 1; i < argc; i++)
    {
      Queue job2;
      int flags, rflags;

      if (commandlinepkgs && commandlinepkgs[i])
	{
	  queue_push2(&job, SOLVER_SOLVABLE, commandlinepkgs[i]);
	  continue;
	}
      queue_init(&job2);
      flags = SELECTION_NAME|SELECTION_PROVIDES|SELECTION_GLOB;
      flags |= SELECTION_CANON|SELECTION_DOTARCH|SELECTION_REL;
      if (kindfilter.count)
	flags |= SELECTION_SKIP_KIND;
      if (mode == MODE_LIST || archfilter_src)
	flags |= SELECTION_WITH_SOURCE;
      if (argv[i][0] == '/')
	flags |= SELECTION_FILELIST | (mode == MODE_ERASE ? SELECTION_INSTALLED_ONLY : 0);
      if (!keyname)
        rflags = selection_make(pool, &job2, argv[i], flags);
      else
	{
	  if (keyname_depstr)
	    flags |= SELECTION_MATCH_DEPSTR;
          rflags = selection_make_matchdeps(pool, &job2, argv[i], flags, pool_str2id(pool, keyname, 1), 0);
	}
      if (repofilter.count)
	selection_filter(pool, &job2, &repofilter);
      if (archfilter.count)
	selection_filter(pool, &job2, &archfilter);
      if (kindfilter.count)
	selection_filter(pool, &job2, &kindfilter);
      if (!job2.count)
	{
	  flags |= SELECTION_NOCASE;
	  if (!keyname)
            rflags = selection_make(pool, &job2, argv[i], flags);
	  else
	    rflags = selection_make_matchdeps(pool, &job2, argv[i], flags, pool_str2id(pool, keyname, 1), 0);
	  if (repofilter.count)
	    selection_filter(pool, &job2, &repofilter);
	  if (archfilter.count)
	    selection_filter(pool, &job2, &archfilter);
	  if (kindfilter.count)
	    selection_filter(pool, &job2, &kindfilter);
	  if (job2.count)
	    printf("[ignoring case for '%s']\n", argv[i]);
	}
      if (!job2.count)
	{
	  fprintf(stderr, "nothing matches '%s'\n", argv[i]);
	  exit(1);
	}
      if (rflags & SELECTION_FILELIST)
        printf("[using file list match for '%s']\n", argv[i]);
      if (rflags & SELECTION_PROVIDES)
	printf("[using capability match for '%s']\n", argv[i]);
      queue_insertn(&job, job.count, job2.count, job2.elements);
      queue_free(&job2);
    }
  keyname = solv_free(keyname);

  if (!job.count && (mainmode == MODE_UPDATE || mainmode == MODE_DISTUPGRADE || mainmode == MODE_VERIFY || repofilter.count || archfilter.count || kindfilter.count))
    {
      queue_push2(&job, SOLVER_SOLVABLE_ALL, 0);
      if (repofilter.count)
	selection_filter(pool, &job, &repofilter);
      if (archfilter.count)
	selection_filter(pool, &job, &archfilter);
      if (kindfilter.count)
	selection_filter(pool, &job, &kindfilter);
    }
  queue_free(&repofilter);
  queue_free(&archfilter);
  queue_free(&kindfilter);

  if (!job.count && mainmode != MODE_PATCH)
    {
      printf("no package matched\n");
      exit(1);
    }

  if (mainmode == MODE_LIST || mainmode == MODE_INFO)
    {
      /* list mode, no solver needed */
      Queue q;
      queue_init(&q);
      for (i = 0; i < job.count; i += 2)
	{
	  int j;
	  queue_empty(&q);
	  pool_job2solvables(pool, &q, job.elements[i], job.elements[i + 1]);
	  for (j = 0; j < q.count; j++)
	    {
	      Solvable *s = pool_id2solvable(pool, q.elements[j]);
	      if (mainmode == MODE_INFO)
		{
		  const char *str;
		  printf("Name:        %s\n", pool_solvable2str(pool, s));
		  printf("Repo:        %s\n", s->repo->name);
		  printf("Summary:     %s\n", solvable_lookup_str(s, SOLVABLE_SUMMARY));
		  str = solvable_lookup_str(s, SOLVABLE_URL);
		  if (str)
		    printf("Url:         %s\n", str);
		  str = solvable_lookup_str(s, SOLVABLE_LICENSE);
		  if (str)
		    printf("License:     %s\n", str);
		  printf("Description:\n%s\n", solvable_lookup_str(s, SOLVABLE_DESCRIPTION));
		  printf("\n");
		}
	      else
		{
#if 1
		  const char *sum = solvable_lookup_str_lang(s, SOLVABLE_SUMMARY, "de", 1);
#else
		  const char *sum = solvable_lookup_str_poollang(s, SOLVABLE_SUMMARY);
#endif
		  printf("  - %s [%s]\n", pool_solvable2str(pool, s), s->repo->name);
		  if (sum)
		    printf("    %s\n", sum);
		}
	    }
	}
      queue_free(&q);
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      solv_free(commandlinepkgs);
      exit(0);
    }

#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)
  if (mainmode == MODE_PATCH)
    add_patchjobs(pool, &job);
#endif

  // add mode
  for (i = 0; i < job.count; i += 2)
    {
      job.elements[i] |= mode;
      if (mode == SOLVER_UPDATE && pool_isemptyupdatejob(pool, job.elements[i], job.elements[i + 1]))
	job.elements[i] ^= SOLVER_UPDATE ^ SOLVER_INSTALL;
      if (cleandeps)
        job.elements[i] |= SOLVER_CLEANDEPS;
      if (forcebest)
        job.elements[i] |= SOLVER_FORCEBEST;
    }

  // multiversion test
  // queue_push2(&job, SOLVER_MULTIVERSION|SOLVER_SOLVABLE_NAME, pool_str2id(pool, "kernel-pae", 1));
  // queue_push2(&job, SOLVER_MULTIVERSION|SOLVER_SOLVABLE_NAME, pool_str2id(pool, "kernel-pae-base", 1));
  // queue_push2(&job, SOLVER_MULTIVERSION|SOLVER_SOLVABLE_NAME, pool_str2id(pool, "kernel-pae-extra", 1));
#if 0
  queue_push2(&job, SOLVER_INSTALL|SOLVER_SOLVABLE_PROVIDES, pool_rel2id(pool, NAMESPACE_LANGUAGE, 0, REL_NAMESPACE, 1));
  queue_push2(&job, SOLVER_ERASE|SOLVER_CLEANDEPS|SOLVER_SOLVABLE_PROVIDES, pool_rel2id(pool, NAMESPACE_LANGUAGE, 0, REL_NAMESPACE, 1));
#endif

rerunsolver:
  solv = solver_create(pool);
  solver_set_flag(solv, SOLVER_FLAG_SPLITPROVIDES, 1);
#if defined(FEDORA) || defined(MAGEIA)
  solver_set_flag(solv, SOLVER_FLAG_ALLOW_VENDORCHANGE, 1);
#endif
  if (mainmode == MODE_ERASE)
    solver_set_flag(solv, SOLVER_FLAG_ALLOW_UNINSTALL, 1);	/* don't nag */
  solver_set_flag(solv, SOLVER_FLAG_BEST_OBEY_POLICY, 1);

  for (;;)
    {
      Id problem, solution;
      int pcnt, scnt;

      pcnt = solver_solve(solv, &job);
      if (testcase)
	{
	  printf("Writing solver testcase:\n");
	  if (!testcase_write(solv, testcase, TESTCASE_RESULT_TRANSACTION | TESTCASE_RESULT_PROBLEMS, 0, 0))
	    printf("%s\n", pool_errstr(pool));
	  testcase = 0;
	}
      if (!pcnt)
	break;
      pcnt = solver_problem_count(solv);
      printf("Found %d problems:\n", pcnt);
      for (problem = 1; problem <= pcnt; problem++)
	{
	  int take = 0;
	  printf("Problem %d/%d:\n", problem, pcnt);
	  solver_printprobleminfo(solv, problem);
	  printf("\n");
	  scnt = solver_solution_count(solv, problem);
	  for (solution = 1; solution <= scnt; solution++)
	    {
	      printf("Solution %d:\n", solution);
	      solver_printsolution(solv, problem, solution);
	      printf("\n");
	    }
	  for (;;)
	    {
	      char inbuf[128], *ip;
	      printf("Please choose a solution: ");
	      fflush(stdout);
	      *inbuf = 0;
	      if (!(ip = fgets(inbuf, sizeof(inbuf), stdin)))
		{
		  printf("Abort.\n");
		  exit(1);
		}
	      while (*ip == ' ' || *ip == '\t')
		ip++;
	      if (*ip >= '0' && *ip <= '9')
		{
		  take = atoi(ip);
		  if (take >= 1 && take <= scnt)
		    break;
		}
	      if (*ip == 's')
		{
		  take = 0;
		  break;
		}
	      if (*ip == 'q')
		{
		  printf("Abort.\n");
		  exit(1);
		}
	    }
	  if (!take)
	    continue;
	  solver_take_solution(solv, problem, take, &job);
	}
    }

  trans = solver_create_transaction(solv);
  if (!trans->steps.count)
    {
      printf("Nothing to do.\n");
      transaction_free(trans);
      solver_free(solv);
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      solv_free(commandlinepkgs);
      exit(1);
    }

  /* display transaction to the user and ask for confirmation */
  printf("\n");
  printf("Transaction summary:\n\n");
  transaction_print(trans);
#if defined(SUSE)
  showdiskusagechanges(trans);
#endif
  printf("install size change: %d K\n", transaction_calc_installsizechange(trans));
  printf("\n");

  acnt = solver_alternatives_count(solv);
  if (acnt)
    {
      if (acnt == 1)
        printf("Have one alternative:\n");
      else
        printf("Have %d alternatives:\n", acnt);
      for (i = 1; i <= acnt; i++)
	{
	  Id id, from;
	  int atype = solver_get_alternative(solv, i, &id, &from, 0, 0, 0);
	  printf("  - %s\n", solver_alternative2str(solv, atype, id, from));
	}
      printf("\n");
      answer = yesno("OK to continue (y/n/a)? ", 'a');
    }
  else
    answer = yesno("OK to continue (y/n)? ", 0);
  if (answer == 'a')
    {
      Queue choicesq;
      Queue answerq;
      Id id, from, chosen;
      int j;

      queue_init(&choicesq);
      queue_init(&answerq);
      for (i = 1; i <= acnt; i++)
	{
	  int atype = solver_get_alternative(solv, i, &id, &from, &chosen, &choicesq, 0);
	  printf("\n%s\n", solver_alternative2str(solv, atype, id, from));
	  for (j = 0; j < choicesq.count; j++)
	    {
	      Id p = choicesq.elements[j];
	      if (p < 0)
		p = -p;
	      queue_push(&answerq, p);
	      printf("%6d: %s\n", answerq.count, pool_solvid2str(pool, p));
	    }
	}
      queue_free(&choicesq);
      printf("\n");
      for (;;)
	{
	  char inbuf[128], *ip;
	  int neg = 0;
	  printf("OK to continue (y/n), or number to change alternative: ");
	  fflush(stdout);
	  *inbuf = 0;
	  if (!(ip = fgets(inbuf, sizeof(inbuf), stdin)))
	    {
	      printf("Abort.\n");
	      exit(1);
	    }
	  while (*ip == ' ' || *ip == '\t')
	    ip++;
	  if (*ip == '-' && ip[1] >= '0' && ip[1] <= '9')
	    {
	      neg = 1;
	      ip++;
	    }
	  if (*ip >= '0' && *ip <= '9')
	    {
	      int take = atoi(ip);
	      if (take > 0 && take <= answerq.count)
		{
		  Id p = answerq.elements[take - 1];
		  queue_free(&answerq);
		  queue_push2(&job, (neg ? SOLVER_DISFAVOR : SOLVER_FAVOR) | SOLVER_SOLVABLE_NAME, pool->solvables[p].name);
		  solver_free(solv);
		  solv = 0;
		  goto rerunsolver;
		  break;
		}
	    }
	  if (*ip == 'n' || *ip == 'y')
	    {
	      answer = *ip == 'n' ? 0 : *ip;
	      break;
	    }
	}
      queue_free(&answerq);
    }
  if (!answer)
    {
      printf("Abort.\n");
      transaction_free(trans);
      solver_free(solv);
      queue_free(&job);
      pool_free(pool);
      free_repoinfos(repoinfos, nrepoinfos);
      solv_free(commandlinepkgs);
      exit(1);
    }

  /* download all new packages */
  queue_init(&checkq);
  newpkgs = transaction_installedresult(trans, &checkq);
  newpkgsfps = 0;
  if (newpkgs)
    {
      int downloadsize = 0;
      for (i = 0; i < newpkgs; i++)
	{
	  Solvable *s;

	  p = checkq.elements[i];
	  s = pool_id2solvable(pool, p);
	  downloadsize += solvable_lookup_sizek(s, SOLVABLE_DOWNLOADSIZE, 0);
	}
      printf("Downloading %d packages, %d K\n", newpkgs, downloadsize);
      newpkgsfps = solv_calloc(newpkgs, sizeof(*newpkgsfps));
      for (i = 0; i < newpkgs; i++)
	{
	  const char *loc;
	  Solvable *s;
	  struct repoinfo *cinfo;

	  p = checkq.elements[i];
	  s = pool_id2solvable(pool, p);
	  if (s->repo == commandlinerepo)
	    {
	      loc = solvable_lookup_location(s, 0);
	      if (!loc)
		continue;
	      if (!(newpkgsfps[i] = fopen(loc, "r")))
		{
		  perror(loc);
		  exit(1);
		}
	      putchar('.');
	      continue;
	    }
	  cinfo = s->repo->appdata;
	  if (!cinfo || cinfo->type == TYPE_INSTALLED)
	    {
	      printf("%s: no repository information\n", s->repo->name);
	      exit(1);
	    }
	  loc = solvable_lookup_location(s, 0);
	  if (!loc)
	     continue;	/* pseudo package? */
#if defined(ENABLE_RPMDB)
	  if (pool->installed && pool->installed->nsolvables)
	    {
	      if ((newpkgsfps[i] = trydeltadownload(s, loc)) != 0)
		{
		  putchar('d');
		  fflush(stdout);
		  continue;		/* delta worked! */
		}
	    }
#endif
	  if ((newpkgsfps[i] = downloadpackage(s, loc)) == 0)
	    {
	      printf("\n%s: %s not found in repository\n", s->repo->name, loc);
	      exit(1);
	    }
	  putchar('.');
	  fflush(stdout);
	}
      putchar('\n');
    }

#if defined(ENABLE_RPMDB) && (defined(SUSE) || defined(FEDORA) || defined(MANDRIVA) || defined(MAGEIA))
  /* check for file conflicts */
  if (newpkgs)
    {
      Queue conflicts;
      queue_init(&conflicts);
      if (checkfileconflicts(pool, &checkq, newpkgs, newpkgsfps, &conflicts))
	{
	  if (yesno("Re-run solver (y/n/q)? ", 0))
	    {
	      for (i = 0; i < newpkgs; i++)
		if (newpkgsfps[i])
		  fclose(newpkgsfps[i]);
	      newpkgsfps = solv_free(newpkgsfps);
	      solver_free(solv);
	      solv = 0;
	      pool_add_fileconflicts_deps(pool, &conflicts);
	      queue_free(&conflicts);
	      goto rerunsolver;
	    }
	}
      queue_free(&conflicts);
    }
#endif

  /* and finally commit the transaction */
  printf("Committing transaction:\n\n");
  transaction_order(trans, 0);
  for (i = 0; i < trans->steps.count; i++)
    {
      int j;
      FILE *fp;
      Id type;

      p = trans->steps.elements[i];
      type = transaction_type(trans, p, SOLVER_TRANSACTION_RPM_ONLY);
      switch(type)
	{
	case SOLVER_TRANSACTION_ERASE:
	  printf("erase %s\n", pool_solvid2str(pool, p));
	  commit_transactionelement(pool, type, p, 0);
	  break;
	case SOLVER_TRANSACTION_INSTALL:
	case SOLVER_TRANSACTION_MULTIINSTALL:
	  printf("install %s\n", pool_solvid2str(pool, p));
	  for (j = 0; j < newpkgs; j++)
	    if (checkq.elements[j] == p)
	      break;
	  fp = j < newpkgs ? newpkgsfps[j] : 0;
	  if (!fp)
	    continue;
	  commit_transactionelement(pool, type, p, fp);
	  fclose(fp);
	  newpkgsfps[j] = 0;
	  break;
	default:
	  break;
	}
    }

  for (i = 0; i < newpkgs; i++)
    if (newpkgsfps[i])
      fclose(newpkgsfps[i]);
  solv_free(newpkgsfps);
  queue_free(&checkq);
  transaction_free(trans);
  solver_free(solv);
  queue_free(&job);
  pool_free(pool);
  free_repoinfos(repoinfos, nrepoinfos);
  solv_free(commandlinepkgs);
  exit(0);
}
