
/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * util.h
 *
 */

#ifndef SATSOLVER_TOOLS_UTIL_H
#define SATSOLVER_TOOLS_UTIL_H

static char *_join_tmp;
static int _join_tmpl;

struct parsedata_common {
  char *tmp;
  int tmpl;
  Pool *pool;
  Repo *repo;
};

static Id
makeevr(Pool *pool, char *s)
{
  if (!strncmp(s, "0:", 2) && s[2])
    s += 2;
  return str2id(pool, s, 1);
}

/**
 * split a string
 */
static int
split(char *l, char **sp, int m)
{
  int i;
  for (i = 0; i < m;)
    {
      while (*l == ' ')
        l++;
      if (!*l)
        break;
      sp[i++] = l;
      while (*l && *l != ' ')
        l++;
      if (!*l)
        break;
      *l++ = 0;
    }
  return i;
}

/* this join does not depend on parsedata */
static char *
join2(const char *s1, const char *s2, const char *s3)
{
  int l = 1;
  char *p;

  if (s1)
    l += strlen(s1);
  if (s2)
    l += strlen(s2);
  if (s3)
    l += strlen(s3);
  if (l > _join_tmpl)
    {
      _join_tmpl = l + 256;
      if (!_join_tmp)
        _join_tmp = malloc(_join_tmpl);
      else
        _join_tmp = realloc(_join_tmp, _join_tmpl);
    }
  p = _join_tmp;
  if (s1)
    {
      strcpy(p, s1);
      p += strlen(s1);
    }
  if (s2)
    {
      strcpy(p, s2);
      p += strlen(s2);
    }
  if (s3)
    {
      strcpy(p, s3);
      p += strlen(s3);
    }
  return _join_tmp;
}

// static char *
// join(struct parsedata_common *pd, const char *s1, const char *s2, const char *s3)
// {
//   int l = 1;
//   char *p;
// 
//   if (s1)
//     l += strlen(s1);
//   if (s2)
//     l += strlen(s2);
//   if (s3)
//     l += strlen(s3);
//   if (l > pd->tmpl)
//     {
//       pd->tmpl = l + 256;
//       if (!pd->tmp)
//         pd->tmp = malloc(pd->tmpl);
//       else
//         pd->tmp = realloc(pd->tmp, pd->tmpl);
//     }
//   p = pd->tmp;
//   if (s1)
//     {
//       strcpy(p, s1);
//       p += strlen(s1);
//     }
//   if (s2)
//     {
//       strcpy(p, s2);
//       p += strlen(s2);
//     }
//   if (s3)
//     {
//       strcpy(p, s3);
//       p += strlen(s3);
//     }
//   return pd->tmp;
// }

/* key Ids */

/* packages */
static Id id_authors;
static Id id_description;
static Id id_diskusage;
static Id id_eula;
static Id id_group;
static Id id_installsize;
static Id id_keywords;
static Id id_license;
static Id id_messagedel;
static Id id_messageins;
static Id id_mediadir;
static Id id_mediafile;
static Id id_medianr;
static Id id_nosource;
static Id id_source;
static Id id_sourceid;
static Id id_time;

/* resobject */
static Id id_summary;
static Id id_description;
// static Id id_insnotify;
// static Id id_delnotify;
static Id id_size;
static Id id_downloadsize;
static Id id_installtime;
static Id id_installonly;

static Id id_isvisible;

/* experimental */
static Id id_must;
static Id id_should;
static Id id_may;

static void init_attr_ids(Pool *pool)
{
  id_size = str2id(pool, "size", 1);;
  id_downloadsize = str2id(pool, "downloadsize", 1);;
  id_installtime = str2id(pool, "installtime", 1);;
  id_installonly = str2id(pool, "installonly", 1);;
  id_summary = str2id(pool, "summary", 1);

  // package
  id_authors = str2id(pool, "authors", 1);
  id_summary = str2id(pool, "summary", 1);
  id_description = str2id(pool, "description", 1);
  id_diskusage = str2id(pool, "diskusage", 1);
  id_downloadsize = str2id(pool, "downloadsize", 1);
  id_eula = str2id(pool, "eula", 1);
  id_group = str2id(pool, "group", 1);
  id_installsize = str2id(pool, "installsize", 1);
  id_keywords = str2id(pool, "keywords", 1);
  id_license = str2id(pool, "license", 1);
  id_messagedel = str2id(pool, "messagedel", 1);
  id_messageins = str2id(pool, "messageins", 1);
  id_mediadir = str2id(pool, "mediadir", 1);
  id_mediafile = str2id(pool, "mediafile", 1);
  id_medianr = str2id(pool, "medianr", 1);
  id_nosource = str2id(pool, "nosource", 1);
  id_source = str2id(pool, "source", 1);
  id_sourceid = str2id(pool, "sourceid", 1);
  id_time = str2id(pool, "time", 1);

  id_isvisible = str2id(pool, "isvisible", 1);

  id_must = str2id(pool, "must", 1);
  id_should = str2id(pool, "should", 1);
  id_may = str2id(pool, "may", 1);

}

/* util function to set a translated string */
void repodata_set_tstr(Repodata *data, Id rid, const char *attrname, const char *lang, const char *str)
{
  Id attrid;
  attrid = str2id(data->repo->pool, join2( attrname, ":", lang), 1);
  repodata_set_str(data, rid, attrid, str);
}

#endif /* SATSOLVER_TOOLS_UTIL_H */
