/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <jansson.h>

static int with_attr = 0;

#include "pool.h"
#include "repo_solv.h"

struct dump_repo_attrs_data {
  const char *previous_key;
  json_t *repository;
};

static int dump_repoattrs_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv);

static void
dump_repodata(Repo *repo, json_t *repository)
{
  unsigned i;
  Repodata *data;
  if (repo->nrepodata == 0)
    return;
  struct dump_repo_attrs_data *cb_data = malloc(sizeof(dump_repoattrs_cb));
  cb_data->previous_key = NULL;
  cb_data->repository = repository;

  json_t *repo_datas = NULL;
  if(repository)
    repo_datas = json_array();
  else
    printf("repo contains %d repodata sections:\n", repo->nrepodata - 1);

  FOR_REPODATAS(repo, i, data)
  {
    json_t *repo_data = NULL;
    json_t *keys      = NULL;
    if (repo_datas) {
      repo_data = json_object();
      keys      = json_array();
    } else
      printf("\nrepodata %d has %d keys, %d schemata\n", i, data->nkeys - 1, data->nschemata - 1);

    unsigned int j;
    for (j = 1; j < data->nkeys; j++) {
      if (keys) {
        json_t *key = json_object();
        json_object_set_new(key, "name", json_string(pool_id2str(repo->pool, data->keys[j].name)));
        json_object_set_new(key, "type", json_string(pool_id2str(repo->pool, data->keys[j].type)));
        json_object_set_new(key, "size", json_integer(data->keys[j].size));
        json_object_set_new(key, "storage", json_integer(data->keys[j].storage));
        json_array_append_new(keys, key);
      } else
        printf("  %s (type %s size %d storage %d)\n", pool_id2str(repo->pool, data->keys[j].name), pool_id2str(repo->pool, data->keys[j].type), data->keys[j].size, data->keys[j].storage);
    }
    if (keys)
      json_object_set_new(repo_data, "keys", keys);
    if (data->localpool) {
      if (repo_data) {
        json_t *strings = json_object();
        json_object_set_new(strings, "number", json_integer(data->spool.nstrings));
        json_object_set_new(strings, "size", json_integer(data->spool.sstrings));
        json_object_set_new(repo_data, "strings", strings);
      } else
        printf("  localpool has %d strings, size is %d\n", data->spool.nstrings, data->spool.sstrings);
    }
    if (data->dirpool.ndirs) {
      if (repo_data)
        json_object_set_new(repo_data, "directories", json_integer(data->dirpool.ndirs));
      else
        printf("  localpool has %d directories\n", data->dirpool.ndirs);
    }
    if (!repository)
      printf("\n");
    repodata_search(data, SOLVID_META, 0, SEARCH_ARRAYSENTINEL|SEARCH_SUB, dump_repoattrs_cb, cb_data);

    if (repo_data)
      json_array_append_new(repo_datas, repo_data);
  }
  if (repository)
    json_object_set_new(repository, "repo_datas", repo_datas);
  else
    printf("\n");
  free(cb_data);
}

#if 0
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
    printf("  %s\n", pool_dep2str(pool, id));
}
#endif

void dump_array_key(const char *keyname, int indent, json_t *json_target) {
  if (json_target) {
    json_t *values = json_array();
    json_object_set_new(json_target, keyname, values);
  } else {
    printf("%s:\n%*s", keyname, indent, "");
  }
}

void dump_array_value(const char *array_key, const char *value, json_t *json_target) {
  if (json_target) {
    json_t *values = json_object_get(json_target, array_key);
    json_array_append_new(values, json_string(value));
  } else {
    printf("  %s\n", value);
  }
}

void dump_checksum(const char *keyname, const char *value, const char *type, json_t* json_target) {
  if(json_target) {
    json_t *checksum_object = json_object();
    json_object_set_new(checksum_object, "value", json_string(value));
    json_object_set_new(checksum_object, "type", json_string(type));
    json_object_set(json_target, keyname, checksum_object);
    json_decref(checksum_object);
  }
  else
    printf("%s: %s (%s)\n", keyname, value, type);
}

void dump_key_llu_value(const char *keyname, long long unsigned int value, json_t *json_target) {
  if(json_target)
    json_object_set_new(json_target, keyname, json_integer(value));
  else
    printf("%s: %llu\n", keyname, value);
}

void dump_key_uint_value(const char *keyname, unsigned int value, json_t *json_target) {
  if(json_target)
    json_object_set_new(json_target, keyname, json_integer(value));
  else
    printf("%s: %u\n", keyname, value);
}

void dump_key_string_value(const char *keyname, const char *value, json_t *json_target) {
  if(json_target)
    json_object_set_new(json_target, keyname, json_string(value));
  else
    printf("%s: %s\n", keyname, value);
}

void dump_key_va_string_value(const char *keyname, json_t *json_target, const char *fmt, ...) {
  char *tmp_str;
  va_list args;
  va_start(args, fmt);

  if (vasprintf(&tmp_str, fmt, args) == -1) {
    fprintf(stderr, "dump_key_va_string_value: error allocating temporary string\n");
    exit(1);
  }

  dump_key_string_value(keyname, tmp_str, json_target);
  free(tmp_str);
  va_end(args);
}

const char*
dump_attr(Repo *repo, Repodata *data, Repokey *key, KeyValue *kv, json_t *json_target, const char *previous_key)
{
  const char *keyname;
  KeyValue *kvp;
  int indent = 0;

  keyname = pool_id2str(repo->pool, key->name);
  for (kvp = kv; (kvp = kvp->parent) != 0; indent += 2)
    printf("  ");
  switch(key->type)
    {
    case REPOKEY_TYPE_ID:
      if (data && data->localpool)
        kv->str = stringpool_id2str(&data->spool, kv->id);
      else
        kv->str = pool_dep2str(repo->pool, kv->id);
      dump_key_string_value(keyname, kv->str, json_target);
      break;
    case REPOKEY_TYPE_CONSTANTID:
      dump_key_string_value(keyname, pool_dep2str(repo->pool, kv->id), json_target);
      break;
    case REPOKEY_TYPE_IDARRAY:
    {
      const char *array_key;
      if (!kv->entry) {
        dump_array_key(keyname, indent, json_target);
        array_key = keyname;
      } else
        array_key = previous_key;

      if (data && data->localpool)
        dump_array_value(array_key, stringpool_id2str(&data->spool, kv->id), json_target);
      else
        dump_array_value(array_key, pool_dep2str(repo->pool, kv->id), json_target);

      break;
    }
    case REPOKEY_TYPE_STR:
      dump_key_string_value(keyname, kv->str, json_target);
      break;
    case REPOKEY_TYPE_MD5:
    case REPOKEY_TYPE_SHA1:
    case REPOKEY_TYPE_SHA256:
      dump_checksum(keyname, repodata_chk2str(data, key->type, (unsigned char *)kv->str), pool_id2str(repo->pool, key->type), json_target);
      break;
    case REPOKEY_TYPE_VOID:
      dump_key_string_value(keyname, "(void)", json_target);
      break;
    case REPOKEY_TYPE_U32:
    case REPOKEY_TYPE_CONSTANT:
      dump_key_uint_value(keyname, kv->num, json_target);
      break;
    case REPOKEY_TYPE_NUM:
      dump_key_llu_value(keyname, SOLV_KV_NUM64(kv), json_target);
      break;
    case REPOKEY_TYPE_BINARY:
      if (kv->num) {
        dump_key_va_string_value(keyname, json_target, "%02x..%02x len %u", (unsigned char)kv->str[0], (unsigned char)kv->str[kv->num - 1], kv->num);
      } else
        dump_key_string_value(keyname, "len 0", json_target);
      break;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
    {
      const char *array_key;
      char tmp_str[100];

      if (!kv->entry) {
        dump_array_key(keyname, indent, json_target);
        array_key = keyname;
      } else
        array_key = previous_key;

      if (snprintf(tmp_str, 100, "%s %u %u\n", repodata_dir2str(data, kv->id, 0), kv->num, kv->num2) == -1) {
        fprintf(stderr, "REPOKEY_TYPE_DIRSTRARRAY: error while creating temporary string\n");
        exit(1);
      }
      dump_array_value(array_key, tmp_str, json_target);
      break;
    }
    case REPOKEY_TYPE_DIRSTRARRAY:
    {
      const char *array_key;
      if (!kv->entry) {
        dump_array_key(keyname, indent, json_target);
        array_key = keyname;
      } else
        array_key = previous_key;

      dump_array_value(array_key, repodata_dir2str(data, kv->id, kv->str), json_target);
      break;
    }
    case REPOKEY_TYPE_FIXARRAY:
    case REPOKEY_TYPE_FLEXARRAY:
      if (!kv->entry) {
        if (json_target)
          json_object_set_new(json_target, keyname, json_array());
        else
          printf("%s:\n", keyname);
      } else if (!json_target)
        printf("\n");
      break;
    default:
      dump_key_string_value(keyname, "?", json_target);
      break;
    }
  return keyname;
}

#if 1
static int
dump_repoattrs_cb(void *vcbdata, Solvable *s, Repodata *data, Repokey *key, KeyValue *kv)
{
  if (key->name == REPOSITORY_SOLVABLES)
    return SEARCH_NEXT_SOLVABLE;

  struct dump_repo_attrs_data * cb_data = (struct dump_repo_attrs_data*) vcbdata;
  cb_data->previous_key = dump_attr(data->repo, data, key, kv, cb_data->repository, cb_data->previous_key);
  return 0;
}
#endif

/*
 * dump all attributes for Id <p>
 */

void
dump_repoattrs(Repo *repo, Id p, json_t* json_target)
{
#if 0
  repo_search(repo, p, 0, 0, SEARCH_ARRAYSENTINEL|SEARCH_SUB, dump_repoattrs_cb, 0);
#else
  Dataiterator di;
  dataiterator_init(&di, repo->pool, repo, p, 0, 0, SEARCH_ARRAYSENTINEL|SEARCH_SUB);
  const char *previous_key = NULL;
  while (dataiterator_step(&di)) {
    previous_key = dump_attr(repo, di.data, di.key, &di.kv, json_target, previous_key);
  }
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


static int
loadcallback(Pool *pool, Repodata *data, void *vdata)
{
  FILE *fp = 0;
  int r;
  const char *location;

  location = repodata_lookup_str(data, SOLVID_META, REPOSITORY_LOCATION);
  if (!location || !with_attr)
    return 0;
  fprintf (stderr, "[Loading SOLV file %s]\n", location);
  fp = fopen (location, "r");
  if (!fp)
    {
      perror(location);
      return 0;
    }
  r = repo_add_solv(data->repo, fp, REPO_USE_LOADING|REPO_LOCALPOOL);
  fclose(fp);
  return !r ? 1 : 0;
}


static void
usage(int status)
{
  fprintf( stderr, "\nUsage:\n"
	   "dumpsolv [-a] [<solvfile>]\n"
	   "  -a  read attributes.\n"
	   "  -j  enable json output.\n"
	   );
  exit(status);
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
		di.kv.str = pool_id2str(repo->pool, di.kv.id);
	      break;
	  case REPOKEY_TYPE_STR:
	  case REPOKEY_TYPE_DIRSTRARRAY:
	      break;
	  default:
	      di.kv.str = 0;
	}
      fprintf (stdout, "found: %d:%s %d %s %d %d %d\n",
	       di.solvid,
	       pool_id2str(repo->pool, di.key->name),
	       di.kv.id,
	       di.kv.str, di.kv.num, di.kv.num2, di.kv.eof);
    }
}
#endif

int main(int argc, char **argv)
{
  Repo *repo;
  Pool *pool;
  int c, i, j, n;
  int exit_code = 0;
  int enable_json_output = 0;
  char *json_output = NULL;
  json_t *json_representation = NULL;
  Solvable *s;

  pool = pool_create();
  pool_setdebuglevel(pool, 1);
  pool_setloadcallback(pool, loadcallback, 0);

  while ((c = getopt(argc, argv, "haj:")) >= 0)
    {
      switch(c)
      {
        case 'h':
          usage(0);
          break;
        case 'a':
          with_attr = 1;
          break;
        case 'j':
          enable_json_output = 1;
          json_output = optarg;
          break;
        default:
          usage(1);
          break;
      }
    }
  for (; optind < argc; optind++)
  {
    if (freopen(argv[optind], "r", stdin) == 0)
    {
      perror(argv[optind]);
      exit(1);
    }
    repo = repo_create(pool, argv[optind]);
    if (repo_add_solv(repo, stdin, 0))
      printf("could not read repository: %s\n", pool_errstr(pool));
  }
  if (!pool->urepos)
  {
    repo = repo_create(pool, argc != 1 ? argv[1] : "<stdin>");
    if (repo_add_solv(repo, stdin, 0))
      printf("could not read repository: %s\n", pool_errstr(pool));
  }
  if (enable_json_output) {
    json_representation = json_object();
    json_t *strings = json_object();
    json_object_set_new(strings, "number", json_integer(pool->ss.nstrings));
    json_object_set_new(strings, "size", json_integer(pool->ss.sstrings));
    json_object_set_new(strings, "rels", json_integer(pool->nrels));
    json_object_set_new(json_representation, "strings", strings);
  } else
    printf("pool contains %d strings, %d rels, string size is %d\n", pool->ss.nstrings, pool->nrels, pool->ss.sstrings);

#if 0
{
  Dataiterator di;
  dataiterator_init(&di, repo, -1, 0, "oo", DI_SEARCHSUB|SEARCH_SUBSTRING);
  while (dataiterator_step(&di))
    dump_attr(di.repo, di.data, di.key, &di.kv, NULL, NULL);
  exit(0);
}
#endif

  json_t *repositories  = NULL;
  if (enable_json_output)
    repositories = json_array();

  n = 0;

  FOR_REPOS(j, repo)
    {
      json_t *resolvables = json_array();
      json_t *repository  = json_object();

      dump_repodata(repo, repository);

      if (!enable_json_output) {
        printf("repo %d contains %d solvables\n", j, repo->nsolvables);
        printf("repo start: %d end: %d\n", repo->start, repo->end);
      }
      FOR_REPO_SOLVABLES(repo, i, s)
	{
	  n++;
    if (!enable_json_output) {
      printf("\n");
      printf("solvable %d (%d):\n", n, i);
    }
#if 0
	  if (s->name || s->evr || s->arch)
	    printf("name: %s %s %s\n", pool_id2str(pool, s->name), pool_id2str(pool, s->evr), pool_id2str(pool, s->arch));
	  if (s->vendor)
	    printf("vendor: %s\n", pool_id2str(pool, s->vendor));
	  printids(repo, "provides", s->provides);
	  printids(repo, "obsoletes", s->obsoletes);
	  printids(repo, "conflicts", s->conflicts);
	  printids(repo, "requires", s->requires);
	  printids(repo, "recommends", s->recommends);
	  printids(repo, "suggests", s->suggests);
	  printids(repo, "supplements", s->supplements);
	  printids(repo, "enhances", s->enhances);
	  if (repo->rpmdbid)
	    printf("rpmdbid: %u\n", repo->rpmdbid[i - repo->start]);
#endif
    json_t* resolvable = NULL;
    if(enable_json_output)
      resolvable = json_object();
    dump_repoattrs(repo, i, resolvable);
    if(enable_json_output) {
      //TODO FIX
      json_array_append(resolvables, resolvable);
      json_decref(resolvable);
    }
#if 0
	  dump_some_attrs(repo, s);
#endif
	}
#if 0
      tryme(repo, 0, SOLVABLE_MEDIANR, 0, 0);
      printf("\n");
      tryme(repo, 0, 0, 0, 0);
      printf("\n");
      tryme(repo, 0, 0, "*y*e*", SEARCH_GLOB);
#endif
      json_object_set_new(repository, "resolvables", resolvables);
      json_array_append_new(repositories, repository);
    }
    if (enable_json_output) {
      json_object_set_new(json_representation, "repositories", repositories);
      if (json_dump_file(json_representation, json_output, JSON_INDENT(2)) == 0)
        printf("\nJSON output saved to %s\n", json_output);
      else {
        fprintf(stderr, "\nError while dumping json data to file\n");
        exit_code = 1;
      }
      json_decref(json_representation);
    }

#if 0
  printf ("\nSearchresults:\n");
  Dataiterator di;
  dataiterator_init(&di, pool, 0, 0, 0, "3", SEARCH_SUB | SEARCH_SUBSTRING | SEARCH_FILES);
  /* int count = 0; */
  while (dataiterator_step(&di))
    {
      printf("%d:", di.solvid);
      dump_attr(repo, di.data, di.key, &di.kv, NULL, NULL);
      /*if (di.solvid == 4 && count++ == 0)
	dataiterator_jump_to_solvable(&di, pool->solvables + 3);*/
      /* dataiterator_skip_attribute(&di); */
      /* dataiterator_skip_solvable(&di); */
      /* dataiterator_skip_repo(&di); */
    }
#endif
  pool_free(pool);
  exit(exit_code);
}
