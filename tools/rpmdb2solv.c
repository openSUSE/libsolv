/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * rpmdb2solv
 * 
 * Reads rpm database (and evtl. more, like product metadata) to build
 * a .solv file of 'installed' solvables.
 * Writes .solv to stdout
 * 
 */

#include <sys/types.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmdb.h"
#ifdef ENABLE_RPMDB_PUBKEY
#include "repo_rpmdb_pubkey.h"
#endif
#include "repo_products.h"
#include "repo_solv.h"
#include "common_write.h"

static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
	  "rpmdb2solv [-n] [-x] [-b <basefile>] [-p <productsdir>] [-r <root>]\n"
	  " -n : No packages, do not read rpmdb, useful to only parse products\n"
	  " -x : use extrapool\n"
	  " -b <basefile> : Write .solv to <basefile>.solv instead of stdout\n"
	  " -p <productsdir> : Scan <productsdir> for .prod files, representing installed products\n"
	  " -r <root> : Prefix rpmdb path and <productsdir> with <root>\n"
	  " -o <solv> : Write .solv to file instead of stdout\n"
	 );
  exit(status);
}


int
main(int argc, char **argv)
{
  Pool *pool = pool_create();
  Repo *repo, *ref = 0;
  Repodata *data;
  int c, percent = 0;
  int extrapool = 0;
  int nopacks = 0;
  const char *root = 0;
  const char *basefile = 0;
  const char *refname = 0;
#ifdef ENABLE_SUSEREPO
  char *proddir = 0;
#endif
  char *outfile = 0;
#ifdef ENABLE_RPMDB_PUBKEY
  int pubkeys = 0;
#endif

  /*
   * parse arguments
   */
  
  while ((c = getopt(argc, argv, "Phnkxb:r:p:o:")) >= 0)
    switch (c)
      {
      case 'h':
	  usage(0);
	break;
      case 'r':
        root = optarg;
        break;
      case 'b':
        basefile = optarg;
        break;
      case 'n':
	nopacks = 1;
	break;
      case 'P':
	percent = 1;
	break;
      case 'p':
#ifdef ENABLE_SUSEREPO
	proddir = optarg;
#endif
	break;
      case 'x':
        extrapool = 1;
        break;
      case 'o':
        outfile = optarg;
        break;
#ifdef ENABLE_RPMDB_PUBKEY
      case 'k':
        nopacks = 1;
        pubkeys = 1;
        break;
#endif
      default:
	usage(1);
      }
  
  if (outfile && !freopen(outfile, "w", stdout))
    {
      perror(outfile);
      exit(1);
    }
    
  /*
   * optional arg is old version of rpmdb solv file
   * should make this a real option instead
   */
  
  if (optind < argc)
    refname = argv[optind];

  if (refname && !nopacks)
    {
      FILE *fp;
      if ((fp = fopen(refname, "r")) == NULL)
        {
          perror(refname);
        }
      else
	{
	  Pool *refpool = extrapool ? pool_create() : 0;
	  ref = repo_create(refpool ? refpool : pool, "ref");
	  if (repo_add_solv(ref, fp, 0) != 0)
	    {
	      fprintf(stderr, "%s: %s\n", refname, pool_errstr(ref->pool));
	      if (ref->pool != pool)
		pool_free(ref->pool);
	      else
		repo_free(ref, 1);
	      ref = 0;
	    }
	  else
	    repo_disable_paging(ref);
	  fclose(fp);
	}
    }

  /*
   * create 'installed' repository
   * add products
   * add rpmdb
   * write .solv
   */

  if (root && *root)
    pool_set_rootdir(pool, root);

  repo = repo_create(pool, "installed");
  data = repo_add_repodata(repo, 0);

  if (!nopacks)
    {
      if (repo_add_rpmdb(repo, ref, REPO_USE_ROOTDIR | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE | (percent ? RPMDB_REPORT_PROGRESS : 0)))
	{
	  fprintf(stderr, "rpmdb2solv: %s\n", pool_errstr(pool));
	  exit(1);
	}
    }
#ifdef ENABLE_RPMDB_PUBKEY
  if (pubkeys)
    {
      if (repo_add_rpmdb_pubkeys(repo, REPO_USE_ROOTDIR | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE))
	{
	  fprintf(stderr, "rpmdb2solv: %s\n", pool_errstr(pool));
	  exit(1);
	}
    }
#endif

#ifdef ENABLE_SUSEREPO
  if (proddir && *proddir)
    {
      if (root && *root)
	{
	  int rootlen = strlen(root);
	  if (!strncmp(root, proddir, rootlen))
	    {
	      proddir += rootlen;
	      if (*proddir != '/' && proddir[-1] == '/')
		proddir--;
	    }
	}
      if (repo_add_products(repo, proddir, REPO_USE_ROOTDIR | REPO_REUSE_REPODATA | REPO_NO_INTERNALIZE))
	{
	  fprintf(stderr, "rpmdb2solv: %s\n", pool_errstr(pool));
	  exit(1);
	}
    }
#endif
  repodata_internalize(data);

  if (ref)
    {
      if (ref->pool != pool)
	pool_free(ref->pool);
      else
	repo_free(ref, 1);
      ref = 0;
    }

  tool_write(repo, basefile, 0);
  pool_free(pool);
  exit(0);
}
