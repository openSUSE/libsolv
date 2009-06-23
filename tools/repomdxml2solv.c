/*
 * Copyright (c) 2007-2009, Novell Inc.
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
#include <unistd.h>

#include "pool.h"
#include "repo.h"
#include "repo_repomdxml.h"
#include "common_write.h"

static void
usage(int status)
{
  fprintf(stderr, "\nUsage:\n"
          "repomdxml2solv [-q query]\n"
	  "  reads a 'repomd.xml' file from <stdin> and writes a .solv file to <stdout>\n"
	  "  -q : query a repomd data entry\n"
	  "  -h : print help & exit\n"
	 );
   exit(status);
}

static void
doquery(Pool *pool, Repo *repo, const char *query)
{
  Id id, type = 0;
  char qbuf[256];
  const char *qp;
  Dataiterator di;

  qp = strchr(query, ':');
  if (qp)
    {
      type = strn2id(pool, query, qp - query, 0);
      if (!type)
	exit(0);
      qp++;
    }
  else
    qp = query;
  snprintf(qbuf, sizeof(qbuf), "repository:repomd:%s", qp);
  id = str2id(pool, qbuf, 0);
  if (!id)
    exit(0);
  dataiterator_init(&di, pool, repo, SOLVID_META, id, 0, 0);
  dataiterator_prepend_keyname(&di, REPOSITORY_REPOMD);
  while (dataiterator_step(&di))
    {
      if (type)
	{
	  dataiterator_setpos_parent(&di);
	  if (pool_lookup_id(pool, SOLVID_POS, REPOSITORY_REPOMD_TYPE) != type)
	    continue;
	}
      switch (di.key->type)
	{
	case REPOKEY_TYPE_ID:
	case REPOKEY_TYPE_CONSTANTID:
	  printf("%s\n", id2str(pool, di.kv.id));
	  break;
	case REPOKEY_TYPE_STR:
	  printf("%s\n", di.kv.str);
	  break;
	case REPOKEY_TYPE_NUM:
	  printf("%d\n", di.kv.num);
	  break;
	case REPOKEY_TYPE_SHA1:
	  printf("sha1:%s\n", repodata_chk2str(di.data, di.key->type, (unsigned char *)di.kv.str));
	  break;
	case REPOKEY_TYPE_SHA256:
	  printf("sha256:%s\n", repodata_chk2str(di.data, di.key->type, (unsigned char *)di.kv.str));
	  break;
	default:
	  break;
	}
    }
  dataiterator_free(&di);
}

int
main(int argc, char **argv)
{
  int c, flags = 0;
  const char *query = 0;
  
  Pool *pool = pool_create();
  Repo *repo = repo_create(pool, "<stdin>");

  while ((c = getopt (argc, argv, "hq:")) >= 0)
    {
      switch(c)
        {
        case 'h':
          usage(0);
          break;
        case 'q':
	  query = optarg;
          break;
	default:
          usage(1);
          break;
        }
    }
  repo_add_repomdxml(repo, stdin, flags);
  if (query)
    doquery(pool, repo, query);
  else
    tool_write(repo, 0, 0);
  pool_free(pool);
  exit(0);
}
