/*
 * repo_appdatadb.c
 *
 * Parses AppSteam Data files.
 * See http://people.freedesktop.org/~hughsient/appdata/
 *
 *
 * Copyright (c) 2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>

#include "pool.h"
#include "repo.h"
#include "util.h"
#include "solv_xmlparser.h"
#include "repo_appdata.h"


enum state {
  STATE_START,
  STATE_APPLICATION,
  STATE_ID,
  STATE_PKGNAME,
  STATE_LICENCE,
  STATE_NAME,
  STATE_SUMMARY,
  STATE_DESCRIPTION,
  STATE_P,
  STATE_UL,
  STATE_UL_LI,
  STATE_OL,
  STATE_OL_LI,
  STATE_URL,
  STATE_GROUP,
  STATE_KEYWORDS,
  STATE_KEYWORD,
  STATE_EXTENDS,
  NUMSTATES
};


static struct solv_xmlparser_element stateswitches[] = {
  { STATE_START,       "applications",  STATE_START,         0 },
  { STATE_START,       "components",    STATE_START,         0 },
  { STATE_START,       "application",   STATE_APPLICATION,   0 },
  { STATE_START,       "component",     STATE_APPLICATION,   0 },
  { STATE_APPLICATION, "id",            STATE_ID,            1 },
  { STATE_APPLICATION, "pkgname",       STATE_PKGNAME,       1 },
  { STATE_APPLICATION, "product_license", STATE_LICENCE,     1 },
  { STATE_APPLICATION, "name",          STATE_NAME,          1 },
  { STATE_APPLICATION, "summary",       STATE_SUMMARY,       1 },
  { STATE_APPLICATION, "description",   STATE_DESCRIPTION,   0 },
  { STATE_APPLICATION, "url",           STATE_URL,           1 },
  { STATE_APPLICATION, "project_group", STATE_GROUP,         1 },
  { STATE_APPLICATION, "keywords",      STATE_KEYWORDS,      0 },
  { STATE_APPLICATION, "extends",       STATE_EXTENDS,       1 },
  { STATE_DESCRIPTION, "p",             STATE_P,             1 },
  { STATE_DESCRIPTION, "ul",            STATE_UL,            0 },
  { STATE_DESCRIPTION, "ol",            STATE_OL,            0 },
  { STATE_UL,          "li",            STATE_UL_LI,         1 },
  { STATE_OL,          "li",            STATE_OL_LI,         1 },
  { STATE_KEYWORDS,    "keyword",       STATE_KEYWORD,       1 },
  { NUMSTATES }
};

struct parsedata {
  Pool *pool;
  Repo *repo;
  Repodata *data;
  int ret;

  Solvable *solvable;
  Id handle;

  int skiplang;
  char *description;
  int licnt;
  int skip_depth;
  int flags;
  char *desktop_file;
  int havesummary;
  const char *filename;
  Queue *owners;

  struct solv_xmlparser xmlp;
};


static void
startElement(struct solv_xmlparser *xmlp, int state, const char *name, const char **atts)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  const char *type;

  /* ignore all language tags */
  if (pd->skiplang || solv_xmlparser_find_attr("xml:lang", atts))
    {
      pd->skiplang++;
      return;
    }

  switch(state)
    {
    case STATE_APPLICATION:
      type = solv_xmlparser_find_attr("type", atts);
      if (!type || !*type)
        type = "desktop";
      if (strcmp(type, "desktop") != 0)
	{
	  /* ignore for now */
	  pd->solvable = 0;
	  break;
	}
      s = pd->solvable = pool_id2solvable(pool, repo_add_solvable(pd->repo));
      pd->handle = s - pool->solvables;
      pd->havesummary = 0;
      repodata_set_poolstr(pd->data, pd->handle, SOLVABLE_CATEGORY, type);
      break;
    case STATE_DESCRIPTION:
      pd->description = solv_free(pd->description);
      break;
    case STATE_OL:
    case STATE_UL:
      pd->licnt = 0;
      break;
    default:
      break;
    }
}

/* replace whitespace with one space/newline */
/* also strip starting/ending whitespace */
static char *
wsstrip(struct parsedata *pd)
{
  struct solv_xmlparser *xmlp = &pd->xmlp;
  int i, j;
  int ws = 0;
  for (i = j = 0; xmlp->content[i]; i++)
    {
      if (xmlp->content[i] == ' ' || xmlp->content[i] == '\t' || xmlp->content[i] == '\n')
	{
	  ws |= xmlp->content[i] == '\n' ? 2 : 1;
	  continue;
	}
      if (ws && j)
	xmlp->content[j++] = (ws & 2) ? '\n' : ' ';
      ws = 0;
      xmlp->content[j++] = xmlp->content[i];
    }
  xmlp->content[j] = 0;
  xmlp->lcontent = j;
  return xmlp->content;
}

/* indent all lines */
static char *
indent(struct parsedata *pd, int il)
{
  struct solv_xmlparser *xmlp = &pd->xmlp;
  int i, l;
  for (l = 0; xmlp->content[l]; )
    {
      if (xmlp->content[l] == '\n')
	{
	  l++;
	  continue;
	}
      if (xmlp->lcontent + il + 1 > xmlp->acontent)
	{
	  xmlp->acontent = xmlp->lcontent + il + 256;
	  xmlp->content = realloc(xmlp->content, xmlp->acontent);
	}
      memmove(xmlp->content + l + il, xmlp->content + l, xmlp->lcontent - l + 1);
      for (i = 0; i < il; i++)
	xmlp->content[l + i] = ' ';
      xmlp->lcontent += il;
      while (xmlp->content[l] && xmlp->content[l] != '\n')
	l++;
    }
  return xmlp->content;
}

static void
add_missing_tags_from_desktop_file(struct parsedata *pd, Solvable *s, const char *desktop_file)
{
  Pool *pool = pd->pool;
  FILE *fp;
  const char *filepath;
  char buf[1024];
  char *p, *p2, *p3;
  int inde = 0;

  filepath = pool_tmpjoin(pool, "/usr/share/applications/", desktop_file, 0);
  if (pd->flags & REPO_USE_ROOTDIR)
    filepath = pool_prepend_rootdir_tmp(pool, filepath);
  if (!(fp = fopen(filepath, "r")))
    return;
  while (fgets(buf, sizeof(buf), fp) > 0)
    {
      int c, l = strlen(buf);
      if (!l)
	continue;
      if (buf[l - 1] != '\n')
	{
	  /* ignore overlong lines */
	  while ((c = getc(fp)) != EOF)
	    if (c == '\n')
	      break;
	  if (c == EOF)
	    break;
	  continue;
	}
      buf[--l] = 0;
      while (l && (buf[l - 1] == ' ' || buf[l - 1] == '\t'))
        buf[--l] = 0;
      p = buf;
      while (*p == ' ' || *p == '\t')
	p++;
      if (!*p || *p == '#')
	continue;
      if (*p == '[')
	inde = 0;
      if (!strcmp(p, "[Desktop Entry]"))
	{
	  inde = 1;
	  continue;
	}
      if (!inde)
	continue;
      p2 = strchr(p, '=');
      if (!p2 || p2 == p)
	continue;
      *p2 = 0;
      for (p3 = p2 - 1; *p3 == ' ' || *p3 == '\t'; p3--)
	*p3 = 0;
      p2++;
      while (*p2 == ' ' || *p2 == '\t')
	p2++;
      if (!*p2)
	continue;
      if (!s->name && !strcmp(p, "Name"))
	s->name = pool_str2id(pool, pool_tmpjoin(pool, "application:", p2, 0), 1);
      else if (!pd->havesummary && !strcmp(p, "Comment"))
	{
	  pd->havesummary = 1;
	  repodata_set_str(pd->data, pd->handle, SOLVABLE_SUMMARY, p2);
	}
      else
	continue;
      if (s->name && pd->havesummary)
	break;	/* our work is done */
    }
  fclose(fp);
}

static char *
guess_filename_from_id(Pool *pool, const char *id)
{
  int l = strlen(id);
  char *r = pool_tmpjoin(pool, id, ".metainfo.xml", 0);
  if (l > 8 && !strcmp(".desktop", id + l - 8))
    strcpy(r + l - 8, ".appdata.xml");
  else if (l > 4 && !strcmp(".ttf", id + l - 4))
    strcpy(r + l - 4, ".metainfo.xml");
  else if (l > 4 && !strcmp(".otf", id + l - 4))
    strcpy(r + l - 4, ".metainfo.xml");
  else if (l > 4 && !strcmp(".xml", id + l - 4))
    strcpy(r + l - 4, ".metainfo.xml");
  else if (l > 3 && !strcmp(".db", id + l - 3))
    strcpy(r + l - 3, ".metainfo.xml");
  else
    return 0;
  return r;
}

static void
endElement(struct solv_xmlparser *xmlp, int state, char *content)
{
  struct parsedata *pd = xmlp->userdata;
  Pool *pool = pd->pool;
  Solvable *s = pd->solvable;
  Id id;

  if (pd->skiplang)
    {
      pd->skiplang--;
      return;
    }
  if (!s)
    return;

  switch (state)
    {
    case STATE_APPLICATION:
      if (!s->arch)
	s->arch = ARCH_NOARCH;
      if (!s->evr)
	s->evr = ID_EMPTY;
      if ((!s->name || !pd->havesummary) && (pd->flags & APPDATA_CHECK_DESKTOP_FILE) != 0 && pd->desktop_file)
	add_missing_tags_from_desktop_file(pd, s, pd->desktop_file);
      if (!s->name && pd->desktop_file)
	{
          char *name = pool_tmpjoin(pool, "application:", pd->desktop_file, 0);
	  int l = strlen(name);
	  if (l > 8 && !strcmp(".desktop", name + l - 8))
	    l -= 8;
	  s->name = pool_strn2id(pool, name, l, 1);
	}
      if (!s->requires && pd->owners)
	{
	  int i;
	  Id id;
	  for (i = 0; i < pd->owners->count; i++)
	    {
	      Solvable *os = pd->pool->solvables + pd->owners->elements[i];
	      s->requires = repo_addid_dep(pd->repo, s->requires, os->name, 0);
	      id = pool_str2id(pd->pool, pool_tmpjoin(pd->pool, "application-appdata(", pool_id2str(pd->pool, os->name), ")"), 1);
	      s->provides = repo_addid_dep(pd->repo, s->provides, id, 0);
	    }
	}
      if (!s->requires && (pd->desktop_file || pd->filename))
	{
	  /* add appdata() link requires/provides */
	  const char *filename = pd->filename;
	  if (!filename)
	    filename = guess_filename_from_id(pool, pd->desktop_file);
	  if (filename)
	    {
	      filename = pool_tmpjoin(pool, "application-appdata(", filename, ")");
	      s->requires = repo_addid_dep(pd->repo, s->requires, pool_str2id(pd->pool, filename + 12, 1), 0);
	      s->provides = repo_addid_dep(pd->repo, s->provides, pool_str2id(pd->pool, filename, 1), 0);
	    }
	}
      if (s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
	s->provides = repo_addid_dep(pd->repo, s->provides, pool_rel2id(pd->pool, s->name, s->evr, REL_EQ, 1), 0);
      pd->solvable = 0;
      pd->desktop_file = solv_free(pd->desktop_file);
      break;
    case STATE_ID:
      pd->desktop_file = solv_strdup(content);
      break;
    case STATE_NAME:
      s->name = pool_str2id(pd->pool, pool_tmpjoin(pool, "application:", content, 0), 1);
      break;
    case STATE_LICENCE:
      repodata_add_poolstr_array(pd->data, pd->handle, SOLVABLE_LICENSE, content);
      break;
    case STATE_SUMMARY:
      pd->havesummary = 1;
      repodata_set_str(pd->data, pd->handle, SOLVABLE_SUMMARY, content);
      break;
    case STATE_URL:
      repodata_set_str(pd->data, pd->handle, SOLVABLE_URL, content);
      break;
    case STATE_GROUP:
      repodata_add_poolstr_array(pd->data, pd->handle, SOLVABLE_GROUP, content);
      break;
    case STATE_EXTENDS:
      repodata_add_poolstr_array(pd->data, pd->handle, SOLVABLE_EXTENDS, content);
      break;
    case STATE_DESCRIPTION:
      if (pd->description)
	{
	  /* strip trailing newlines */
	  int l = strlen(pd->description);
	  while (l && pd->description[l - 1] == '\n')
	    pd->description[--l] = 0;
          repodata_set_str(pd->data, pd->handle, SOLVABLE_DESCRIPTION, pd->description);
	}
      break;
    case STATE_P:
      content = wsstrip(pd);
      pd->description = solv_dupappend(pd->description, content, "\n\n");
      break;
    case STATE_UL_LI:
      wsstrip(pd);
      content = indent(pd, 4);
      content[2] = '-';
      pd->description = solv_dupappend(pd->description, content, "\n");
      break;
    case STATE_OL_LI:
      wsstrip(pd);
      content = indent(pd, 4);
      if (++pd->licnt >= 10)
	content[0] = '0' + (pd->licnt / 10) % 10;
      content[1] = '0' + pd->licnt  % 10;
      content[2] = '.';
      pd->description = solv_dupappend(pd->description, content, "\n");
      break;
    case STATE_UL:
    case STATE_OL:
      pd->description = solv_dupappend(pd->description, "\n", 0);
      break;
    case STATE_PKGNAME:
      id = pool_str2id(pd->pool, content, 1);
      s->requires = repo_addid_dep(pd->repo, s->requires, id, 0);
      id = pool_str2id(pd->pool, pool_tmpjoin(pd->pool, "application-appdata(", content, ")"), 1);
      s->provides = repo_addid_dep(pd->repo, s->provides, id, 0);
      break;
    case STATE_KEYWORD:
      repodata_add_poolstr_array(pd->data, pd->handle, SOLVABLE_KEYWORDS, content);
      break;
    default:
      break;
    }
}

static void
errorCallback(struct solv_xmlparser *xmlp, const char *errstr, unsigned int line, unsigned int column)
{
  struct parsedata *pd = xmlp->userdata;
  pool_debug(pd->pool, SOLV_ERROR, "repo_appdata: %s at line %u:%u\n", errstr, line, column);
  pd->ret = -1;
  if (pd->solvable)
    {   
      repo_free_solvable(pd->repo, pd->solvable - pd->pool->solvables, 1); 
      pd->solvable = 0;
    }
}

static int
repo_add_appdata_fn(Repo *repo, FILE *fp, int flags, const char *filename, Queue *owners)
{
  Repodata *data;
  struct parsedata pd;

  data = repo_add_repodata(repo, flags);
  memset(&pd, 0, sizeof(pd));
  pd.repo = repo;
  pd.pool = repo->pool;
  pd.data = data;
  pd.flags = flags;
  pd.filename = filename;
  pd.owners = owners;

  solv_xmlparser_init(&pd.xmlp, stateswitches, &pd, startElement, endElement, errorCallback);
  solv_xmlparser_parse(&pd.xmlp, fp);
  solv_xmlparser_free(&pd.xmlp);

  solv_free(pd.desktop_file);
  solv_free(pd.description);

  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);

  return pd.ret;
}

int
repo_add_appdata(Repo *repo, FILE *fp, int flags)
{
  return repo_add_appdata_fn(repo, fp, flags, 0, 0);
}

static void
search_uninternalized_filelist(Repo *repo, const char *dir, Queue *res)
{
  Pool *pool = repo->pool;
  Id rdid, p;
  Id iter, did, idid;

  for (rdid = 1; rdid < repo->nrepodata; rdid++)
    {
      Repodata *data = repo_id2repodata(repo, rdid);
      if (!data)
	continue;
      if (data->state == REPODATA_STUB)
	continue;
      if (!repodata_has_keyname(data, SOLVABLE_FILELIST))
	continue;
      did = repodata_str2dir(data, dir, 0);
      if (!did)
	continue;
      for (p = data->start; p < data->end; p++)
	{
	  if (p >= pool->nsolvables)
	    continue;
	  if (pool->solvables[p].repo != repo)
	    continue;
	  iter = 0;
	  for (;;)
	    {
	      const char *str;
	      int l;
	      Id id;
	      idid = did;
	      str = repodata_lookup_dirstrarray_uninternalized(data, p, SOLVABLE_FILELIST, &idid, &iter);
	      if (!iter)
		break;
	      l = strlen(str);
	      if (l > 12 && strncmp(str + l - 12, ".appdata.xml", 12))
		id = pool_str2id(pool, str, 1);
	      else if (l > 13 && strncmp(str + l - 13, ".metainfo.xml", 13))
		id = pool_str2id(pool, str, 1);
	      else
		continue;
	      queue_push2(res, p, id);
	    }
	}
    }
}

/* add all files ending in .appdata.xml */
int
repo_add_appdata_dir(Repo *repo, const char *appdatadir, int flags)
{
  DIR *dir;
  char *dirpath;
  Repodata *data;
  Queue flq;
  Queue oq;

  queue_init(&flq);
  queue_init(&oq);
  if (flags & APPDATA_SEARCH_UNINTERNALIZED_FILELIST)
    search_uninternalized_filelist(repo, appdatadir, &flq);
  data = repo_add_repodata(repo, flags);
  if (flags & REPO_USE_ROOTDIR)
    dirpath = pool_prepend_rootdir(repo->pool, appdatadir);
  else
    dirpath = solv_strdup(appdatadir);
  if ((dir = opendir(dirpath)) != 0)
    {
      struct dirent *entry;
      while ((entry = readdir(dir)))
	{
	  const char *n;
	  FILE *fp;
	  int len = strlen(entry->d_name);
	  if (entry->d_name[0] == '.')
	    continue;
	  if (!(len > 12 && !strcmp(entry->d_name + len - 12, ".appdata.xml")) &&
	      !(len > 13 && !strcmp(entry->d_name + len - 13, ".metainfo.xml")))
	    continue;
          n = pool_tmpjoin(repo->pool, dirpath, "/", entry->d_name);
	  fp = fopen(n, "r");
	  if (!fp)
	    {
	      pool_error(repo->pool, 0, "%s: %s", n, strerror(errno));
	      continue;
	    }
	  if (flags & APPDATA_SEARCH_UNINTERNALIZED_FILELIST)
	    {
	      Id id = pool_str2id(repo->pool, entry->d_name, 0);
	      queue_empty(&oq);
	      if (id)
		{
		  int i;
		  for (i = 0; i < flq.count; i += 2)
		    if (flq.elements[i + 1] == id)
		      queue_push(&oq, flq.elements[i]);
		}
	    }
	  repo_add_appdata_fn(repo, fp, flags | REPO_NO_INTERNALIZE | REPO_REUSE_REPODATA | APPDATA_CHECK_DESKTOP_FILE, entry->d_name, oq.count ? &oq : 0);
	  fclose(fp);
	}
      closedir(dir);
    }
  solv_free(dirpath);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  queue_free(&oq);
  queue_free(&flq);
  return 0;
}
