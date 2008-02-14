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
#if 0
#include "attr_store.h"
#include "attr_store_p.h"

static void
dump_attrs_1 (Attrstore *s, unsigned int entry)
{
  attr_iterator ai;
  FOR_ATTRS (s, entry, &ai)
    {
      fprintf (stdout, "%s:", id2str (s->pool, ai.name));
      switch (ai.type)
	{
	case TYPE_ATTR_INT:
	  fprintf (stdout, "int  %u\n", ai.as_int);
	  break;
	case TYPE_ATTR_CHUNK:
	  {
	    const char *str = attr_retrieve_blob (s, ai.as_chunk[0], ai.as_chunk[1]);
	    if (str)
	      fprintf (stdout, "blob %s\n", str);
	    else
	      fprintf (stdout, "blob %u+%u\n", ai.as_chunk[0], ai.as_chunk[1]);
	  }
	  break;
	case TYPE_ATTR_STRING:
	  fprintf (stdout, "str  %s\n", ai.as_string);
	  break;
	case TYPE_ATTR_INTLIST:
	  {
	    fprintf (stdout, "lint\n ");
	    while (1)
	      {
		int val;
		get_num (ai.as_numlist, val);
		fprintf (stdout, " %d", (val & 63) | ((val >> 1) & ~63));
		if (!(val & 64))
		  break;
	      }
	    fprintf (stdout, "\n");
	    break;
	  }
	case TYPE_ATTR_LOCALIDS:
	  {
	    fprintf (stdout, "lids");
	    while (1)
	      {
		Id val;
		get_num (ai.as_numlist, val);
		if (!val)
		  break;
		fprintf (stdout, "\n  %s(%d)", localid2str (s, val), val);
	      }
	    fprintf (stdout, "\n");
	    break;
	  }
	default:
	  fprintf (stdout, "\n");
	  break;
	}
    }
}

static void
dump_attrs (Repo *repo, unsigned int entry)
{
  unsigned i;
  for (i = 0; i < repo->nrepodata; i++)
    {
      Attrstore *s = repo->repodata[i].s;
      if (s && entry < s->entries)
        dump_attrs_1 (s, entry);
    }
}
#endif

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
        printf("\n  %s", id2str(repo->pool, data->keys[j].name));
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
    default:
      printf("%s: ?\n", keyname);
      break;
    }
  return 0;
}

void
dump_repoattrs(Repo *repo, Id p)
{
  int i;
  Repodata *data;
  for (i = 0, data = repo->repodata; i < repo->nrepodata; i++, data++)
    {
      if (data->state == REPODATA_STUB || data->state == REPODATA_ERROR)
        continue;
      if (p < data->start || p >= data->end)
	continue;
      repodata_search(data, p - data->start, 0, dump_repoattrs_cb, 0);
    }
}

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
        printf("name: %s(%s) %s %s\n", id2str(pool, s->name) + s->kind, id2str(pool, s->name), id2str(pool, s->evr), id2str(pool, s->arch));
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
#if 0
      dump_attrs (repo, n - 1);
#endif
      dump_repoattrs(repo, i);
#if 1
      dump_some_attrs(repo, s);
#endif
      n++;
    }
  pool_free(pool);
  exit(0);
}
