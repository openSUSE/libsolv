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
      printf("%s has %d keys, %d schemata\n", data->location ? data->location : "**EMBED**", data->nkeys, data->nschemata);
      for (j = 1; j < data->nkeys; j++)
        printf("  %s (type %s size %d storage %d)\n", id2str(repo->pool, data->keys[j].name), id2str(repo->pool, data->keys[j].type), data->keys[j].size, data->keys[j].storage);
      if (data->localpool)
	printf("  localpool has %d strings, size is %d\n", data->spool.nstrings, data->spool.sstrings);
      if (data->dirpool.ndirs)
	printf("  localpool has %d directories\n", data->dirpool.ndirs);
      if (data->addedfileprovides)
	{
	  printf("  added file provides:\n");
	  for (j = 0; data->addedfileprovides[j]; j++)
	    printf("    %s\n", id2str(repo->pool, data->addedfileprovides[j]));
	}
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

int
dump_attr(Repo *repo, Repodata *data, Repokey *key, KeyValue *kv)
{
  const char *keyname;

  keyname = id2str(repo->pool, key->name);
  switch(key->type)
    {
    case REPOKEY_TYPE_ID:
      if (data && data->localpool)
	kv->str = stringpool_id2str(&data->spool, kv->id);
      else
        kv->str = id2str(repo->pool, kv->id);
      printf("%s: %s\n", keyname, kv->str);
      break;
    case REPOKEY_TYPE_CONSTANTID:
      printf("%s: %s\n", keyname, dep2str(repo->pool, kv->id));
      break;
    case REPOKEY_TYPE_IDARRAY:
      if (data && data->localpool)
        printf("%s: %s\n", keyname, stringpool_id2str(&data->spool, kv->id));
      else
        printf("%s: %s\n", keyname, dep2str(repo->pool, kv->id));
      break;
    case REPOKEY_TYPE_STR:
      printf("%s: %s\n", keyname, kv->str);
      break;
    case REPOKEY_TYPE_MD5:
    case REPOKEY_TYPE_SHA1:
    case REPOKEY_TYPE_SHA256:
      printf("%s: %s\n", keyname, repodata_chk2str(data, key->type, (unsigned char *)kv->str));
      break;
    case REPOKEY_TYPE_VOID:
      printf("%s: (void)\n", keyname);
      break;
    case REPOKEY_TYPE_U32:
    case REPOKEY_TYPE_NUM:
    case REPOKEY_TYPE_CONSTANT:
      printf("%s: %d\n", keyname, kv->num);
      break;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      printf("%s: %s %d %d\n", keyname, repodata_dir2str(data, kv->id, 0), kv->num, kv->num2);
      break;
    case REPOKEY_TYPE_DIRSTRARRAY:
      printf("%s: %s\n", keyname, repodata_dir2str(data, kv->id, kv->str));
      break;
    default:
      printf("%s: ?\n", keyname);
      break;
    }
  return 0;
}

static int
dump_repoattrs_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  return dump_attr(s->repo, data, key, kv);
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
    dump_attr(repo, di.data, di.key, &di.kv);
#endif
}

#if 0
void
dump_some_attrs(Repo *repo, Solvable *s)
{
  const char *summary = 0;
  unsigned int medianr = -1, downloadsize = -1;
  unsigned int time = -1;
  summary = repo_lookup_str(s, SOLVABLE_SUMMARY);
  medianr = repo_lookup_num(s, SOLVABLE_MEDIANR);
  downloadsize = repo_lookup_num (s, SOLVABLE_DOWNLOADSIZE);
  time = repo_lookup_num(s, SOLVABLE_BUILDTIME);
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
	  case REPOKEY_TYPE_ID:
	  case REPOKEY_TYPE_IDARRAY:
	      if (di.data && di.data->localpool)
		di.kv.str = stringpool_id2str(&di.data->spool, di.kv.id);
	      else
		di.kv.str = id2str(repo->pool, di.kv.id);
	      break;
	  case REPOKEY_TYPE_STR:
	  case REPOKEY_TYPE_DIRSTRARRAY:
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
  int i, j, n;
  Solvable *s;
  
  pool = pool_create();
  pool_setdebuglevel(pool, 1);
  pool_setloadcallback(pool, loadcallback, 0);

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
	  repo = repo_create(pool, argv[0]);
	  if (repo_add_solv(repo, stdin))
	    printf("could not read repository\n");
	}
      argv++;
    }

  if (!pool->nrepos)
    {
      repo = repo_create(pool, argc != 1 ? argv[1] : "<stdin>");
      if (repo_add_solv(repo, stdin))
	printf("could not read repository\n");
    }
  printf("pool contains %d strings, %d rels, string size is %d\n", pool->ss.nstrings, pool->nrels, pool->ss.sstrings);
  for (j = 0; 1 && j < pool->nrepos; j++)
    {
      repo = pool->repos[j];
      dump_repodata(repo);
      printf("repo %d contains %d solvables %d non-solvables\n", j, repo->nsolvables, repo->nextra);
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
      for (i = 0; i < repo->nextra; i++)
	{
	  printf("\nextra %d:\n", i);
	  Dataiterator di;
	  dataiterator_init(&di, repo, -1 - i, 0, 0, SEARCH_EXTRA | SEARCH_NO_STORAGE_SOLVABLE);
	  while (dataiterator_step(&di))
	    dump_attr(repo, di.data, di.key, &di.kv);
	}
#if 0
      tryme(repo, 0, SOLVABLE_MEDIANR, 0, 0);
      printf("\n");
      tryme(repo, 0, 0, 0, 0);
      printf("\n");
      tryme(repo, 0, 0, "*y*e*", SEARCH_GLOB);
#endif
    }
#if 0
  printf ("\nSearchresults:\n");
  Dataiterator di;
  dataiterator_init(&di, pool->repos[0], 0, 0, "3", SEARCH_EXTRA | SEARCH_SUBSTRING | SEARCH_ALL_REPOS);
  //int count = 0;
  while (dataiterator_step(&di))
    {
      printf("%d:", di.solvid);
      dump_attr(repo, di.data, di.key, &di.kv);
      /*if (di.solvid == 4 && count++ == 0)
	dataiterator_jump_to_solvable(&di, pool->solvables + 3);*/
      //dataiterator_skip_attribute(&di);
      //dataiterator_skip_solvable(&di);
      //dataiterator_skip_repo(&di);
    }
#endif
  pool_free(pool);
  exit(0);
}
