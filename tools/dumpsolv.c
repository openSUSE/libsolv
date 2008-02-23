/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static int with_attr = 0;

#include "pool.h"
#include "repo_solv.h"

static void
dump_repodata (Repo *repo)
{
  unsigned i;
  Repodata *data;
  if (repo->nrepodata == 0)
    return;
  printf("repo refers to %d subfiles:\n", repo->nrepodata);
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      unsigned int j;
      printf("%s has %d keys", data->location ? data->location : "**EMBED**", data->nkeys);
      for (j = 1; j < data->nkeys; j++)
        printf("\n  %s (type %d size %d storage %d)", id2str(repo->pool, data->keys[j].name), data->keys[j].type, data->keys[j].size, data->keys[j].storage);
      printf("\n");
    }
  printf("\n");
}

static void
printids(Repo *repo, char *kind, Offset ido)
{
  Pool *pool = repo->pool;
  Id id, *ids;
  if (!ido)
    return;
  printf("%s:\n", kind);
  ids = repo->idarraydata + ido;
  while((id = *ids++) != 0)
    printf("  %s\n", dep2str(pool, id));
}

static void
printdir(Repodata *data, Id dir)
{
  Id comp;
  Id parent = dirpool_parent(&data->dirpool, dir);
  if (parent)
    {
      printdir(data, parent);
      putchar('/');
    }
  comp = dirpool_compid(&data->dirpool, dir);
  if (data->localpool)
    printf("%s", stringpool_id2str(&data->spool, comp));
  else
    printf("%s", id2str(data->repo->pool, comp));
}

int
dump_repoattrs_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  const char *keyname;

  keyname = id2str(data->repo->pool, key->name);
  switch(key->type)
    {
    case TYPE_ID:
      if (data->localpool)
	kv->str = stringpool_id2str(&data->spool, kv->id);
      else
        kv->str = id2str(data->repo->pool, kv->id);
      printf("%s: %s\n", keyname, kv->str);
      break;
    case TYPE_STR:
      printf("%s: %s\n", keyname, kv->str);
      break;
    case TYPE_VOID:
      printf("%s\n", keyname);
      break;
    case TYPE_NUM:
    case TYPE_CONSTANT:
      printf("%s: %d\n", keyname, kv->num);
      break;
    case TYPE_DIRNUMNUMARRAY:
      printf("%s: ", keyname);
      printdir(data, kv->id);
      printf(" %d %d\n", kv->num, kv->num2);
      break;
    case TYPE_DIRSTRARRAY:
      printf("%s: ", keyname);
      printdir(data, kv->id);
      printf("/%s\n", kv->str);
      break;
    default:
      printf("%s: ?\n", keyname);
      break;
    }
  return 0;
}

/*
 * dump all attributes for Id <p>
 */

void
dump_repoattrs(Repo *repo, Id p)
{
#if 1
  repo_search(repo, p, 0, 0, SEARCH_NO_STORAGE_SOLVABLE, dump_repoattrs_cb, 0);
#else
  Dataiterator di;
  dataiterator_init(&di, repo, p, 0, 0, SEARCH_NO_STORAGE_SOLVABLE);
  while (dataiterator_step(&di))
    dump_repoattrs_cb(0, repo->pool->solvables + di.solvid, di.data, di.key,
		      &di.kv);
#endif
}

#if 0
void
dump_some_attrs(Repo *repo, Solvable *s)
{
  Id name = str2id (repo->pool, "summary", 0);
  const char *summary = 0;
  unsigned int medianr = -1, downloadsize = -1;
  unsigned int time = -1;
  if (name)
    summary = repo_lookup_str (s, name);
  if ((name = str2id (repo->pool, "medianr", 0)))
    medianr = repo_lookup_num (s, name);
  if ((name = str2id (repo->pool, "downloadsize", 0)))
    downloadsize = repo_lookup_num (s, name);
  if ((name = str2id (repo->pool, "time", 0)))
    time = repo_lookup_num (s, name);

  printf ("  XXX %d %d %u %s\n", medianr, downloadsize, time, summary);
}
#endif


static FILE *
loadcallback (Pool *pool, Repodata *data, void *vdata)
{
  FILE *fp = 0;
  if (data->location && with_attr)
    {
      fprintf (stderr, "Loading SOLV file %s\n", data->location);
      fp = fopen (data->location, "r");
      if (!fp)
	perror(data->location);
    }
  return fp;
}


static void
usage( const char *err )
{
  if (err)
    fprintf (stderr, "\n** Error:\n  %s\n", err);
  fprintf( stderr, "\nUsage:\n"
	   "dumpsolv [-a] [<solvfile>]\n"
	   "  -a  read attributes.\n"
	   );
  exit(0);
}

#if 0
static void
tryme (Repo *repo, Id p, Id keyname, const char *match, int flags)
{
  Dataiterator di;
  dataiterator_init(&di, repo, p, keyname, match, flags);
  while (dataiterator_step(&di))
    {
      switch (di.key->type)
	{
	  case TYPE_ID:
	  case TYPE_IDARRAY:
	      if (di.data && di.data->localpool)
		di.kv.str = stringpool_id2str(&di.data->spool, di.kv.id);
	      else
		di.kv.str = id2str(repo->pool, di.kv.id);
	      break;
	  case TYPE_STR:
	  case TYPE_DIRSTRARRAY:
	      break;
	  default:
	      di.kv.str = 0;
	}
      fprintf (stdout, "found: %d:%s %d %s %d %d %d\n",
	       di.solvid,
	       id2str(repo->pool, di.key->name),
	       di.kv.id,
	       di.kv.str, di.kv.num, di.kv.num2, di.kv.eof);
    }
}
#endif

int main(int argc, char **argv)
{
  Repo *repo;
  Pool *pool;
  int i, n;
  Solvable *s;
  
  argv++;
  argc--;
  while (argc--)
    {
      const char *s = argv[0];
      if (*s++ == '-')
        while (*s)
          switch (*s++)
	    {
	      case 'h': usage(NULL); break;
	      case 'a': with_attr = 1; break;
	      default : break;
	    }
      else
	{
	  if (freopen (argv[0], "r", stdin) == 0)
	    {
	      perror(argv[0]);
	      exit(1);
	    }
	  break;
	}
      argv++;
    }

  pool = pool_create();
  pool_setdebuglevel(pool, 1);
  pool_setloadcallback(pool, loadcallback, 0);

  repo = repo_create(pool, argc != 1 ? argv[1] : "<stdin>");
  if (repo_add_solv(repo, stdin))
    printf("could not read repository\n");
  dump_repodata (repo);
  printf("repo contains %d solvables\n", repo->nsolvables);
  for (i = repo->start, n = 1; i < repo->end; i++)
    {
      s = pool->solvables + i;
      if (s->repo != repo)
	continue;
      printf("\n");
      printf("solvable %d:\n", n);
      if (s->name || s->evr || s->arch)
        printf("name: %s %s %s\n", id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
      if (s->vendor)
        printf("vendor: %s\n", id2str(pool, s->vendor));
      printids(repo, "provides", s->provides);
      printids(repo, "obsoletes", s->obsoletes);
      printids(repo, "conflicts", s->conflicts);
      printids(repo, "requires", s->requires);
      printids(repo, "recommends", s->recommends);
      printids(repo, "suggests", s->suggests);
      printids(repo, "supplements", s->supplements);
      printids(repo, "enhances", s->enhances);
      printids(repo, "freshens", s->freshens);
      if (repo->rpmdbid)
	printf("rpmdbid: %u\n", repo->rpmdbid[i - repo->start]);
#if 0
      dump_attrs (repo, n - 1);
#endif
      dump_repoattrs(repo, i);
#if 0
      dump_some_attrs(repo, s);
#endif
      n++;
    }
#if 0
  tryme(repo, 0, str2id (repo->pool, "medianr", 0), 0, 0);
  printf("\n");
  tryme(repo, 0, 0, 0, 0);
  printf("\n");
  tryme(repo, 0, 0, "*y*e*", SEARCH_GLOB);
#endif
  pool_free(pool);
  exit(0);
}
