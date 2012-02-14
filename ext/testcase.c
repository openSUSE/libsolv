/*
 * Copyright (c) 2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "pool.h"
#include "poolarch.h"
#include "poolvendor.h"
#include "repo.h"
#include "repo_susetags.h"
#include "solver.h"
#include "solverdebug.h"
#include "chksum.h"
#include "testcase.h"
#include "solv_xfopen.h"


static struct job2str {
  Id job;
  const char *str;
} job2str[] = {
  { SOLVER_NOOP,          "noop" },
  { SOLVER_INSTALL,       "install" },
  { SOLVER_ERASE,         "erase" },
  { SOLVER_UPDATE,        "update" },
  { SOLVER_WEAKENDEPS,    "weakendeps" },
  { SOLVER_NOOBSOLETES,   "noobsoletes" },
  { SOLVER_LOCK,          "lock" },
  { SOLVER_DISTUPGRADE,   "distupgrade" },
  { SOLVER_VERIFY,        "verify" },
  { SOLVER_DROP_ORPHANED, "droporphaned" },
  { SOLVER_USERINSTALLED, "userinstalled" },
  { 0, 0 }
};

static struct jobflags2str {
  Id flag;
  const char *str;
} jobflags2str[] = {
  { SOLVER_WEAK,      "weak" },
  { SOLVER_ESSENTIAL, "essential" },
  { SOLVER_CLEANDEPS, "cleandeps" },
  { SOLVER_SETEV,     "setev" },
  { SOLVER_SETEVR,    "setevr" },
  { SOLVER_SETARCH,   "setarch" },
  { SOLVER_SETVENDOR, "setvendor" },
  { SOLVER_SETREPO,   "setrepo" },
  { SOLVER_NOAUTOSET, "noautoset" },
  { 0, 0 }
};

static struct solverflags2str {
  Id flag;
  const char *str;
  int def;
} solverflags2str[] = {
  { SOLVER_FLAG_ALLOW_DOWNGRADE,            "allowdowngrade", 0 },
  { SOLVER_FLAG_ALLOW_ARCHCHANGE,           "allowarchchange", 0 },
  { SOLVER_FLAG_ALLOW_VENDORCHANGE,         "allowvendorchange", 0 },
  { SOLVER_FLAG_ALLOW_UNINSTALL,            "allowuninstall", 0 },
  { SOLVER_FLAG_NO_UPDATEPROVIDE,           "noupdateprovide", 0 },
  { SOLVER_FLAG_SPLITPROVIDES,              "splitprovides", 0 },
  { SOLVER_FLAG_IGNORE_RECOMMENDED,         "ignorerecommended", 0 },
  { SOLVER_FLAG_IGNORE_ALREADY_RECOMMENDED, "ignorealreadyrecommended", 0 },
  { SOLVER_FLAG_NO_INFARCHCHECK,            "noinfarchcheck", 0 },
  { 0, 0, 0 }
};


typedef struct strqueue {
  char **str;
  int nstr;
} Strqueue;

#define STRQUEUE_BLOCK 63

static void
strqueue_init(Strqueue *q)
{
  q->str = 0;
  q->nstr = 0;
}

static void
strqueue_free(Strqueue *q)
{
  int i;
  for (i = 0; i < q->nstr; i++)
    solv_free(q->str[i]);
  q->str = solv_free(q->str);
  q->nstr = 0;
}

static void
strqueue_push(Strqueue *q, const char *s)
{
  q->str = solv_extend(q->str, q->nstr, 1, sizeof(*q->str), STRQUEUE_BLOCK);
  q->str[q->nstr++] = solv_strdup(s);
}

static void
strqueue_pushjoin(Strqueue *q, const char *s1, const char *s2, const char *s3)
{
  q->str = solv_extend(q->str, q->nstr, 1, sizeof(*q->str), STRQUEUE_BLOCK);
  q->str[q->nstr++] = solv_dupjoin(s1, s2, s3);
}

static int
strqueue_sort_cmp(const void *ap, const void *bp, void *dp)
{
  const char *a = *(const char **)ap;
  const char *b = *(const char **)bp;
  return strcmp(a ? a : "", b ? b : "");
}

static void
strqueue_sort(Strqueue *q)
{
  if (q->nstr > 1)
    solv_sort(q->str, q->nstr, sizeof(*q->str), strqueue_sort_cmp, 0);
}

static void
strqueue_sort_u(Strqueue *q)
{
  int i, j;
  strqueue_sort(q);
  for (i = j = 0; i < q->nstr; i++)
    if (!j || strqueue_sort_cmp(q->str + i, q->str + j - 1, 0) != 0)
      q->str[j++] = q->str[i];
  q->nstr = j;
}

static char *
strqueue_join(Strqueue *q)
{
  int i, l = 0;
  char *r, *rp;
  for (i = 0; i < q->nstr; i++)
    if (q->str[i])
      l += strlen(q->str[i]) + 1;
  l++;	/* trailing \0 */
  r = solv_malloc(l);
  rp = r;
  for (i = 0; i < q->nstr; i++)
    if (q->str[i])
      {
        strcpy(rp, q->str[i]);
        rp += strlen(rp);
	*rp++ = '\n';
      }
  *rp = 0;
  return r;
}

static void
strqueue_split(Strqueue *q, const char *s)
{
  const char *p;
  while ((p = strchr(s, '\n')) != 0)
    {
      q->str = solv_extend(q->str, q->nstr, 1, sizeof(*q->str), STRQUEUE_BLOCK);
      q->str[q->nstr] = solv_malloc(p - s + 1);
      if (p > s)
	memcpy(q->str[q->nstr], s, p - s);
      q->str[q->nstr][p - s] = 0;
      q->nstr++;
      s = p + 1;
    }
  if (*s)
    strqueue_push(q, s);
}

static void
strqueue_diff(Strqueue *sq1, Strqueue *sq2, Strqueue *osq)
{
  int i = 0, j = 0;
  while (i < sq1->nstr && j < sq2->nstr)
    {
      int r = strqueue_sort_cmp(sq1->str + i, sq2->str + j, 0);
      if (!r)
	i++, j++;
      else if (r < 0)
	strqueue_pushjoin(osq, "-", sq1->str[i++], 0);
      else
	strqueue_pushjoin(osq, "+", sq2->str[j++], 0);
    }
  while (i < sq1->nstr)
    strqueue_pushjoin(osq, "-", sq1->str[i++], 0);
  while (j < sq2->nstr)
    strqueue_pushjoin(osq, "+", sq2->str[j++], 0);
}

static inline int
pool_isknownarch(Pool *pool, Id id)
{
  if (!id || id == ID_EMPTY)
    return 0;
  if (id == ARCH_SRC || id == ARCH_NOSRC || id == ARCH_NOARCH)
    return 1;
  if (pool->id2arch && (id > pool->lastarch || !pool->id2arch[id]))
    return 0;
  return 1;
}

Id
testcase_str2dep(Pool *pool, char *s)
{
  char *n, *a;
  Id id;
  int flags;

  if ((n = strchr(s, '|')) != 0)
    {    
      id = testcase_str2dep(pool, n + 1);
      *n = 0; 
      id = pool_rel2id(pool, testcase_str2dep(pool, s), id, REL_OR, 1);
      *n = '|'; 
      return id;
    }
  while (*s == ' ' || *s == '\t')
    s++;
  n = s;
  while (*s && *s != ' ' && *s != '\t' && *s != '<' && *s != '=' && *s != '>')
    s++;
  if ((a = strchr(n, '.')) != 0 && a + 1 < s)
    {
      Id archid = pool_strn2id(pool, a + 1, s - (a + 1), 0);
      if (pool_isknownarch(pool, archid))
	{
          id = pool_strn2id(pool, n, a - n, 1);
	  id = pool_rel2id(pool, id, archid, REL_ARCH, 1);
	}
      else
        id = pool_strn2id(pool, n, s - n, 1);
    }
  else
    id = pool_strn2id(pool, n, s - n, 1);
  if (!*s)
    return id;
  while (*s == ' ' || *s == '\t')
    s++;
  flags = 0;
  for (;;s++)
    {  
      if (*s == '<')
        flags |= REL_LT;
      else if (*s == '=')
        flags |= REL_EQ;
      else if (*s == '>')
        flags |= REL_GT;
      else
        break;
    }
  if (!flags)
    return id;
  while (*s == ' ' || *s == '\t')
    s++;
  n = s;
  while (*s && *s != ' ' && *s != '\t')
    s++;
  return pool_rel2id(pool, id, pool_strn2id(pool, n, s - n, 1), flags, 1);
}

const char *
testcase_repoid2str(Pool *pool, Id repoid)
{
  Repo *repo = pool_id2repo(pool, repoid);
  if (repo->name)
    {
      char *r = pool_tmpjoin(pool, repo->name, 0, 0);
      char *rp;
      for (rp = r; *rp; rp++)
	if (*rp == ' ' || *rp == '\t')
	  *rp = '_';
      return r;
    }
  else
    {
      char buf[20];
      sprintf(buf, "@#%d", repoid);
      return pool_tmpjoin(pool, buf, 0, 0);
    }
}

const char *
testcase_solvid2str(Pool *pool, Id p)
{
  Solvable *s = pool->solvables + p;
  const char *str;
  char buf[20];

  if (p == SYSTEMSOLVABLE)
    return "@SYSTEM";
  str = pool_solvid2str(pool, p);
  if (!s->repo)
    return pool_tmpappend(pool, str, "@", 0);
  if (s->repo->name)
    {
      int l = strlen(str);
      char *str2 = pool_tmpappend(pool, str, "@", s->repo->name);
      for (; str2[l]; l++)
	if (str2[l] == ' ' || str2[l] == '\t')
	  str2[l] = '_';
      return str2;
    }
  sprintf(buf, "@#%d", s->repo->repoid);
  return pool_tmpappend(pool, str, buf, 0);
}

Repo *
testcase_str2repo(Pool *pool, const char *str)
{
  int repoid;
  Repo *repo = 0;
  if (str[0] == '#' && (str[1] >= '0' && str[1] <= '9'))
    {
      int j;
      repoid = 0;
      for (j = 1; str[j] >= '0' && str[j] <= '9'; j++)
	repoid = repoid * 10 + (str[j] - '0');
      if (!str[j] && repoid > 0 && repoid < pool->nrepos)
	repo = pool_id2repo(pool, repoid);
    }
  if (!repo)
    {
      FOR_REPOS(repoid, repo)
	{
	  int i, l;
	  if (!repo->name)
	    continue;
	  l = strlen(repo->name);
	  for (i = 0; i < l; i++)
	    {
	      int c = repo->name[i];
	      if (c == ' ' || c == '\t')
		c = '_';
	      if (c != str[i])
		break;
	    }
	  if (i == l && !str[l])
	    break;
	}
      if (repoid >= pool->nrepos)
	repo = 0;
    }
  return repo;
}

Id
testcase_str2solvid(Pool *pool, const char *str)
{
  int i, l = strlen(str);
  int repostart;
  Repo *repo;
  Id arch;

  if (!l)
    return 0;
  if (*str == '@' && !strcmp(str, "@SYSTEM"))
    return SYSTEMSOLVABLE;
  repo = 0;
  for (i = l - 1; i >= 0; i--)
    if (str[i] == '@' && (repo = testcase_str2repo(pool, str + i + 1)) != 0)
      break;
  if (i < 0)
    i = l;
  repostart = i;
  /* now find the arch (if present) */
  arch = 0;
  for (i = repostart - 1; i > 0; i--)
    if (str[i] == '.')
      {
	arch = pool_strn2id(pool, str + i + 1, repostart - (i + 1), 0);
	if (arch)
	  repostart = i;
	break;
      }
  /* now find the name */
  for (i = repostart - 1; i > 0; i--)
    {
      if (str[i] == '-')
	{
	  Id nid, evrid, p, pp;
	  nid = pool_strn2id(pool, str, i, 0);
	  if (!nid)
	    continue;
	  evrid = pool_strn2id(pool, str + i + 1, repostart - (i + 1), 0);
	  if (!evrid)
	    continue;
	  FOR_PROVIDES(p, pp, nid)
	    {
	      Solvable *s = pool->solvables + p;
	      if (s->name != nid || s->evr != evrid)
		continue;
	      if (repo && s->repo != repo)
		continue;
	      if (arch && s->arch != arch)
		continue;
	      return p;
	    }
	}
    }
  return 0;
}

const char *
testcase_job2str(Pool *pool, Id how, Id what)
{
  char *ret;
  const char *jobstr;
  const char *selstr;
  const char *pkgstr;
  int i, o;
  Id select = how & SOLVER_SELECTMASK;

  for (i = 0; job2str[i].str; i++)
    if ((how & SOLVER_JOBMASK) == job2str[i].job)
      break;
  jobstr = job2str[i].str ? job2str[i].str : "unknown";
  if (select == SOLVER_SOLVABLE)
    {
      selstr = " pkg ";
      pkgstr = testcase_solvid2str(pool, what);
    }
  else if (select == SOLVER_SOLVABLE_NAME)
    {
      selstr = " name ";
      pkgstr = pool_dep2str(pool, what);
    }
  else if (select == SOLVER_SOLVABLE_PROVIDES)
    {
      selstr = " provides ";
      pkgstr = pool_dep2str(pool, what);
    }
  else if (select == SOLVER_SOLVABLE_ONE_OF)
    {
      Id p;
      selstr = " oneof ";
      pkgstr = 0;
      while ((p = pool->whatprovidesdata[what++]) != 0)
	{
	  const char *s = testcase_solvid2str(pool, p);
	  if (pkgstr)
	    {
	      pkgstr = pool_tmpappend(pool, pkgstr, " ", s);
	      pool_freetmpspace(pool, s);
	    }
	  else
	    pkgstr = s;
	}
      if (!pkgstr)
	pkgstr = "nothing";
    }
  else if (select == SOLVER_SOLVABLE_REPO)
    {
      Repo *repo = pool_id2repo(pool, what);
      selstr = " repo ";
      if (!repo->name)
	{
          char buf[20];
	  sprintf(buf, "#%d", repo->repoid);
	  pkgstr = pool_tmpjoin(pool, buf, 0, 0);
	}
      else
        pkgstr = pool_tmpjoin(pool, repo->name, 0, 0);
    }
  else if (select == SOLVER_SOLVABLE_ALL)
    {
      selstr = " all ";
      pkgstr = "packages";
    }
  else
    {
      selstr = " unknown ";
      pkgstr = "unknown";
    }
  ret = pool_tmpjoin(pool, jobstr, selstr, pkgstr);
  o = strlen(ret);
  ret = pool_tmpappend(pool, ret, " ", 0);
  for (i = 0; jobflags2str[i].str; i++)
    if ((how & jobflags2str[i].flag) != 0)
      ret = pool_tmpappend(pool, ret, ",", jobflags2str[i].str);
  if (!ret[o + 1])
    ret[o] = 0;
  else
    {
      ret[o + 1] = '[';
      ret = pool_tmpappend(pool, ret, "]", 0);
    }
  return ret;
}

Id
testcase_str2job(Pool *pool, const char *str, Id *whatp)
{
  int i;
  Id job;
  Id what;
  char *s;
  char **pieces = 0;
  int npieces = 0;

  *whatp = 0;
  /* so we can patch it */
  s = pool_tmpjoin(pool, str, 0, 0);
  /* split it in pieces */
  for (;;)
    {
      while (*s == ' ' || *s == '\t')
	s++;
      if (!*s)
	break;
      pieces = solv_extend(pieces, npieces, 1, sizeof(*pieces), 7);
      pieces[npieces++] = s;
      while (*s && *s != ' ' && *s != '\t')
	s++;
      if (*s)
	*s++ = 0;
    }
  if (npieces < 3)
    {
      pool_debug(pool, SOLV_ERROR, "str2job: bad line '%s'\n", str);
      return 0;
    }

  for (i = 0; job2str[i].str; i++)
    if (!strcmp(pieces[0], job2str[i].str))
      break;
  if (!job2str[i].str)
    {
      pool_debug(pool, SOLV_ERROR, "str2job: unknown job '%s'\n", str);
      return 0;
    }
  job = job2str[i].job;
  if (npieces > 3)
    {
      char *flags = pieces[npieces - 1];
      char *nf;
      if (*flags == '[' && flags[strlen(flags) - 1] == ']')
	{
	  npieces--;
	  flags++;
	  flags[strlen(flags) - 1] = ',';
	  while (*flags)
	    {
	      for (nf = flags; *nf != ','; nf++)
		;
	      *nf++ = 0;
	      for (i = 0; jobflags2str[i].str; i++)
		if (!strcmp(flags, jobflags2str[i].str))
		  break;
	      if (!jobflags2str[i].str)
		{
		  pool_debug(pool, SOLV_ERROR, "str2job: unknown jobflags in '%s'\n", str);
		  return 0;
		}
	      job |= jobflags2str[i].flag;
	      flags = nf;
	    }
	}
    }
  if (!strcmp(pieces[1], "pkg"))
    {
      if (npieces != 3)
	{
	  pool_debug(pool, SOLV_ERROR, "str2job: bad pkg selector in '%s'\n", str);
	  return 0;
	}
      job |= SOLVER_SOLVABLE;
      what = testcase_str2solvid(pool, pieces[2]);
      if (!what)
	{
	  pool_debug(pool, SOLV_ERROR, "str2job: unknown package '%s'\n", pieces[2]);
	  return 0;
	}
    }
  else if (!strcmp(pieces[1], "name") || !strcmp(pieces[1], "provides"))
    {
      /* join em again for dep2str... */
      char *sp;
      for (sp = pieces[2]; sp < pieces[npieces - 1]; sp++)
	if (*sp == 0)
	  *sp = ' ';
      what = testcase_str2dep(pool, pieces[2]);
      if (pieces[1][0] == 'n')
	job |= SOLVER_SOLVABLE_NAME;
      else
	job |= SOLVER_SOLVABLE_PROVIDES;
    }
  else if (!strcmp(pieces[1], "oneof"))
    {
      Queue q;
      job |= SOLVER_SOLVABLE_ONE_OF;
      queue_init(&q);
      if (npieces > 3 && strcmp(pieces[2], "nothing") != 0)
	{
	  for (i = 2; i < npieces; i++)
	    {
	      Id p = testcase_str2solvid(pool, pieces[i]);
	      if (!p)
		{
		  pool_debug(pool, SOLV_ERROR, "str2job: unknown package '%s'\n", pieces[i]);
		  queue_free(&q);
		  return 0;
		}
	      queue_push(&q, p);
	    }
	}
      what = pool_queuetowhatprovides(pool, &q);
      queue_free(&q);
    }
  else if (!strcmp(pieces[1], "repo"))
    {
      Repo *repo;
      if (npieces != 3)
	{
	  pool_debug(pool, SOLV_ERROR, "str2job: bad line '%s'\n", str);
	  return 0;
	}
      repo = testcase_str2repo(pool, pieces[2]);
      if (!repo)
	{
	  pool_debug(pool, SOLV_ERROR, "str2job: unknown repo '%s'\n", pieces[2]);
	  return 0;
	}
      job |= SOLVER_SOLVABLE_REPO;
      what = repo->repoid;
    }
  else if (!strcmp(pieces[1], "all"))
    {
      if (npieces != 3 && strcmp(pieces[2], "packages") != 0)
	{
	  pool_debug(pool, SOLV_ERROR, "str2job: bad line '%s'\n", str);
	  return 0;
	}
      job |= SOLVER_SOLVABLE_ALL;
      what = 0;
    }
  else
    {
      pool_debug(pool, SOLV_ERROR, "str2job: unknown selection in '%s'\n", str);
      return 0;
    }
  *whatp = what;
  return job;
}

static void
writedeps(Repo *repo, FILE *fp, const char *tag, Id key, Solvable *s, Offset off)
{
  Pool *pool = repo->pool;
  Id id, *dp, *prvdp;
  int tagwritten = 0;
  const char *idstr;

  if (!off)
    return;
  dp = repo->idarraydata + off;
  prvdp = 0;
  while ((id = *dp++) != 0)
    {
      if (key == SOLVABLE_REQUIRES && id == SOLVABLE_PREREQMARKER)
	{
	  if (tagwritten)
	    fprintf(fp, "-%s\n", tag);
	  tagwritten = 0;
	  tag = "Prq:";
	  continue;
	}
      if (key == SOLVABLE_PROVIDES && id == SOLVABLE_FILEMARKER)
	{
	  prvdp = dp + 1;
	  continue;
	}
      idstr = pool_dep2str(pool, id);
      if (ISRELDEP(id))
	{
	  Reldep *rd = GETRELDEP(pool, id);
	  if (key == SOLVABLE_CONFLICTS && rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_OTHERPROVIDERS)
	    {
	      if (!strncmp(idstr, "namespace:", 10))
		idstr += 10;
	    }
	  if (key == SOLVABLE_SUPPLEMENTS)
	    {
	      if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_FILESYSTEM)
		{
		  if (!strncmp(idstr, "namespace:", 10))
		    idstr += 10;
		}
	      else if (rd->flags == REL_NAMESPACE && rd->name == NAMESPACE_MODALIAS)
		{
		  if (!strncmp(idstr, "namespace:", 10))
		    idstr += 10;
		}
	      else if (rd->flags == REL_AND)
		{
		  /* either packageand chain or modalias */
		  idstr = 0;
		  if (ISRELDEP(rd->evr))
		    {
		      Reldep *mrd = GETRELDEP(pool, rd->evr);
		      if (mrd->flags == REL_NAMESPACE && mrd->name == NAMESPACE_MODALIAS)
			{
			  idstr = pool_tmpjoin(pool, "modalias(", pool_dep2str(pool, rd->name), ":");
			  idstr = pool_tmpappend(pool, idstr, pool_dep2str(pool, mrd->evr), ")");
			}
		      else if (mrd->flags >= 8)
			continue;
		    }
		  if (!idstr)
		    {
		      /* must be and chain */
		      idstr = pool_dep2str(pool, rd->evr);
		      for (;;)
			{
			  id = rd->name;
			  if (!ISRELDEP(id))
			    break;
			  rd = GETRELDEP(pool, id);
			  if (rd->flags != REL_AND)
			    break;
			  idstr = pool_tmpjoin(pool, pool_dep2str(pool, rd->evr), ":", idstr);
			}
		      idstr = pool_tmpjoin(pool, pool_dep2str(pool, id), ":", idstr);
		      idstr = pool_tmpjoin(pool, "packageand(", idstr, ")");
		    }
		}
	      else if (rd->flags >= 8)
		continue;
	    }
	}
      if (!tagwritten)
	{
	  fprintf(fp, "+%s\n", tag);
	  tagwritten = 1;
	}
      fprintf(fp, "%s\n", idstr);
    }
  if (key == SOLVABLE_PROVIDES)
    {
      /* add the filelist */
      Dataiterator di;
      dataiterator_init(&di, pool, repo, s - pool->solvables, SOLVABLE_FILELIST, 0, 0);
      while (dataiterator_step(&di))
	{
	  const char *s = repodata_dir2str(di.data, di.kv.id, di.kv.str);
	  if (prvdp)
	    {
	      Id id = pool_str2id(pool, s, 0);
	      if (id)
		{
		  for (dp = prvdp; *dp; dp++)
		    if (*dp == id)
		      break;
		  if (*dp)
		    continue;	/* already included */
		}
	    }
	  if (!tagwritten)
	    {
	      fprintf(fp, "+%s", tag);
	      tagwritten = 1;
	    }
	  fprintf(fp, "%s\n", s);
	}
    }
  if (tagwritten)
    fprintf(fp, "-%s\n", tag);
}

int
testcase_write_susetags(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  Solvable *s;
  Id p;
  const char *name;
  const char *evr;
  const char *arch;
  const char *release;
#if 0
  const char *chksum;
  Id chksumtype, type;
  unsigned int medianr;
#endif
  const char *tmp;
  unsigned int ti;

  fprintf(fp, "=Ver: 2.0\n");
  FOR_REPO_SOLVABLES(repo, p, s)
    {
      name = pool_id2str(pool, s->name);
      evr = pool_id2str(pool, s->evr);
      arch = pool_id2str(pool, s->arch);
      release = strrchr(evr, '-');
      if (!release)
	release = evr + strlen(evr);
      fprintf(fp, "=Pkg: %s %.*s %s %s\n", name, release - evr, evr, *release && release[1] ? release + 1 : "0", arch);
      tmp = solvable_lookup_str(s, SOLVABLE_SUMMARY);
      if (tmp)
        fprintf(fp, "=Sum: %s\n", tmp);
#if 0
      chksum = solvable_lookup_checksum(s, SOLVABLE_CHECKSUM, &chksumtype);
      if (chksum)
	fprintf(fp, "=Cks: %s %s\n", solv_chksum_type2str(chksumtype), chksum);
#endif
      writedeps(repo, fp, "Req:", SOLVABLE_REQUIRES, s, s->requires);
      writedeps(repo, fp, "Prv:", SOLVABLE_PROVIDES, s, s->provides);
      writedeps(repo, fp, "Obs:", SOLVABLE_OBSOLETES, s, s->obsoletes);
      writedeps(repo, fp, "Con:", SOLVABLE_CONFLICTS, s, s->conflicts);
      writedeps(repo, fp, "Rec:", SOLVABLE_RECOMMENDS, s, s->recommends);
      writedeps(repo, fp, "Sup:", SOLVABLE_SUPPLEMENTS, s, s->supplements);
      writedeps(repo, fp, "Sug:", SOLVABLE_SUGGESTS, s, s->suggests);
      writedeps(repo, fp, "Enh:", SOLVABLE_ENHANCES, s, s->enhances);
#if 0
      tmp = solvable_lookup_str(s, SOLVABLE_GROUP);
      if (tmp)
	fprintf(fp, "=Grp: %s\n", tmp);
      tmp = solvable_lookup_str(s, SOLVABLE_LICENSE);
      if (tmp)
	fprintf(fp, "=Lic: %s\n", tmp);
#endif
      if (s->vendor)
	fprintf(fp, "=Vnd: %s\n", pool_id2str(pool, s->vendor));
#if 0
      type = solvable_lookup_type(s, SOLVABLE_SOURCENAME);
      if (type)
	{
	  if (type != REPOKEY_TYPE_VOID)
	    name = solvable_lookup_str(s, SOLVABLE_SOURCENAME);
	  type = solvable_lookup_type(s, SOLVABLE_SOURCEEVR);
	  if (type)
	    {
	      if (type != REPOKEY_TYPE_VOID)
		evr = solvable_lookup_str(s, SOLVABLE_SOURCEEVR);
	      release = strrchr(evr, '-');
	      if (!release)
		release = evr + strlen(evr);
	      fprintf(fp, "=Src: %s %.*s %s %s\n", name, release - evr, evr, *release && release[1] ? release + 1 : "0", solvable_lookup_str(s, SOLVABLE_SOURCEARCH));
	    }
	}
#endif
      ti = solvable_lookup_num(s, SOLVABLE_BUILDTIME, 0);
      if (ti)
	fprintf(fp, "=Tim: %u\n", ti);
#if 0
      tmp = solvable_get_location(s, &medianr);
      if (tmp)
	{
	  const char *base = strrchr(tmp, '/');
	  if (!base)
            fprintf(fp, "=Loc: %d %s\n", medianr, tmp);
	  else if (strlen(arch) == base - tmp && !strncmp(tmp, arch, base - tmp))
            fprintf(fp, "=Loc: %d %s\n", medianr, base + 1);
	  else
            fprintf(fp, "=Loc: %d %s %.*s\n", medianr, base + 1, base - tmp, tmp);
	}
#endif
    }
  return 0;
}

const char *
testcase_getsolverflags(Solver *solv)
{
  Pool *pool = solv->pool;
  const char *str = 0;
  int i, v;
  for (i = 0; solverflags2str[i].str; i++)
    {
      v = solver_get_flag(solv, solverflags2str[i].flag);
      if (v == solverflags2str[i].def)
	continue;
      str = pool_tmpappend(pool, str, v ? " " : " !", solverflags2str[i].str);
    }
  return str ? str + 1 : "";
}

int
testcase_setsolverflags(Solver *solv, const char *str)
{
  const char *p = str, *s;
  int i, v;
  for (;;)
    {
      while (*p == ' ' || *p == '\t' || *p == ',')
	p++;
      v = 1;
      if (*p == '!')
	{
	  p++;
	  v = 0;
	}
      if (!*p)
	break;
      s = p;
      while (*p && *p != ' ' && *p != '\t' && *p != ',')
	p++;
      for (i = 0; solverflags2str[i].str; i++)
	if (!strncmp(solverflags2str[i].str, s, p - s) && solverflags2str[i].str[p - s] == 0)
	  break;
      if (!solverflags2str[i].str)
	{
	  pool_debug(solv->pool, SOLV_ERROR, "setsolverflags: unknown flag '%.*s'\n", p - s, s);
	  return 0;
	}
      solver_set_flag(solv, solverflags2str[i].flag, v);
    }
  return 1;
}

void
testcase_resetsolverflags(Solver *solv)
{
  int i;
  for (i = 0; solverflags2str[i].str; i++)
    solver_set_flag(solv, solverflags2str[i].flag, solverflags2str[i].def);
}

static const char *
testcase_ruleid(Solver *solv, Id rid)
{
  Strqueue sq;
  Queue q;
  int i;
  void *chk;
  const unsigned char *md5;
  int md5l;
  const char *s;

  queue_init(&q);
  strqueue_init(&sq);
  solver_ruleliterals(solv, rid, &q);
  for (i = 0; i < q.count; i++)
    {
      Id p = q.elements[i];
      s = testcase_solvid2str(solv->pool, p > 0 ? p : -p);
      if (p < 0)
	s = pool_tmpjoin(solv->pool, "!", s, 0);
      strqueue_push(&sq, s);
    }
  queue_free(&q);
  strqueue_sort_u(&sq);
  chk = solv_chksum_create(REPOKEY_TYPE_MD5);
  for (i = 0; i < sq.nstr; i++)
    solv_chksum_add(chk, sq.str[i], strlen(sq.str[i]) + 1);
  md5 = solv_chksum_get(chk, &md5l);
  s = pool_bin2hex(solv->pool, md5, md5l);
  chk = solv_chksum_free(chk, 0);
  strqueue_free(&sq);
  return s;
}

static const char *
testcase_problemid(Solver *solv, Id problem)
{
  Strqueue sq;
  Queue q;
  void *chk;
  const unsigned char *md5;
  int i, md5l;
  const char *s;

  /* we build a hash of all rules that define the problem */
  queue_init(&q);
  strqueue_init(&sq);
  solver_findallproblemrules(solv, problem, &q);
  for (i = 0; i < q.count; i++)
    strqueue_push(&sq, testcase_ruleid(solv, q.elements[i]));
  queue_free(&q);
  strqueue_sort_u(&sq);
  chk = solv_chksum_create(REPOKEY_TYPE_MD5);
  for (i = 0; i < sq.nstr; i++)
    solv_chksum_add(chk, sq.str[i], strlen(sq.str[i]) + 1);
  md5 = solv_chksum_get(chk, &md5l);
  s = pool_bin2hex(solv->pool, md5, 4);
  chk = solv_chksum_free(chk, 0);
  strqueue_free(&sq);
  return s;
}

static const char *
testcase_solutionid(Solver *solv, Id problem, Id solution)
{
  Id intid;
  void *chk;
  const unsigned char *md5;
  int md5l;
  const char *s;

  intid = solver_solutionelement_internalid(solv, problem, solution);
  /* internal stuff! handle with care! */
  if (intid < 0)
    {
      /* it's a job */
      s = testcase_job2str(solv->pool, solv->job.elements[-intid - 1], solv->job.elements[-intid]);
    }
  else
    {
      /* it's a rule */
      s = testcase_ruleid(solv, intid);
    }
  chk = solv_chksum_create(REPOKEY_TYPE_MD5);
  solv_chksum_add(chk, s, strlen(s) + 1);
  md5 = solv_chksum_get(chk, &md5l);
  s = pool_bin2hex(solv->pool, md5, 4);
  chk = solv_chksum_free(chk, 0);
  return s;
}

static struct class2str {
  Id class;
  const char *str;
} class2str[] = {
  { SOLVER_TRANSACTION_ERASE,          "erase" },
  { SOLVER_TRANSACTION_INSTALL,        "install" },
  { SOLVER_TRANSACTION_REINSTALLED,    "reinstall" },
  { SOLVER_TRANSACTION_DOWNGRADED,     "downgrade" },
  { SOLVER_TRANSACTION_CHANGED,        "change" },
  { SOLVER_TRANSACTION_UPGRADED,       "upgrade" },
  { SOLVER_TRANSACTION_OBSOLETED,      "obsolete" },
  { SOLVER_TRANSACTION_MULTIINSTALL,   "multiinstall" },
  { SOLVER_TRANSACTION_MULTIREINSTALL, "multireinstall" },
  { 0, 0 }
};

char *
testcase_solverresult(Solver *solv)
{
  Pool *pool = solv->pool;
  Transaction *trans;
  Queue q;
  int i, j;
  Id p, op;
  const char *s;
  char *probprefix, *solprefix, *result;
  int problem, solution, element;
  int pcnt, scnt;
  Strqueue sq;

  strqueue_init(&sq);
  trans = solver_create_transaction(solv);
  queue_init(&q);
  for (i = 0; class2str[i].str; i++)
    {
      queue_empty(&q);
      transaction_classify_pkgs(trans, 0, class2str[i].class, 0, 0, &q);
      for (j = 0; j < q.count; j++)
	{
	  p = q.elements[j];
          op = 0;
	  if (pool->installed && pool->solvables[p].repo == pool->installed)
	    op = transaction_obs_pkg(trans, p);
	  s = pool_tmpjoin(pool, class2str[i].str, " ", testcase_solvid2str(pool, p));
	  if (op)
	    s = pool_tmpjoin(pool, s, " ", testcase_solvid2str(pool, op));
	  strqueue_push(&sq, s);
	}
    }
  queue_free(&q);
  transaction_free(trans);
  pcnt = solver_problem_count(solv);
  for (problem = 1; problem <= pcnt; problem++)
    {
      Id rid, from, to, dep;
      SolverRuleinfo rinfo;
      rid = solver_findproblemrule(solv, problem);
      s = testcase_problemid(solv, problem);
      probprefix = solv_dupjoin("problem ", s, 0);
      rinfo = solver_ruleinfo(solv, rid, &from, &to, &dep);
      s = pool_tmpjoin(pool, probprefix, " info ", solver_problemruleinfo2str(solv, rinfo, from, to, dep));
      strqueue_push(&sq, s);
      scnt = solver_solution_count(solv, problem);
      for (solution = 1; solution <= scnt; solution++)
	{
	  s = testcase_solutionid(solv, problem, solution);
	  solprefix = solv_dupjoin(probprefix, " solution ", s);
	  element = 0;
	  while ((element = solver_next_solutionelement(solv, problem, solution, element, &p, &op)) != 0)
	    {
	      if (p == SOLVER_SOLUTION_JOB)
		s = pool_tmpjoin(pool, solprefix, " deljob ", testcase_job2str(pool, solv->job.elements[op - 1], solv->job.elements[op]));
	      else if (p > 0 && op == 0)
	        s = pool_tmpjoin(pool, solprefix, " erase ", testcase_solvid2str(pool, p));
	      else if (p > 0 && op > 0)
		{
	          s = pool_tmpjoin(pool, solprefix, " replace ", testcase_solvid2str(pool, p));
	          s = pool_tmpappend(pool, s, " ", testcase_solvid2str(pool, op));
		}
	      else if (p < 0 && op > 0)
	        s = pool_tmpjoin(pool, solprefix, " allow ", testcase_solvid2str(pool, op));
	      else
	        s = pool_tmpjoin(pool, solprefix, " unknown", 0);
	      strqueue_push(&sq, s);
	    }
	  solv_free(solprefix);
	}
      solv_free(probprefix);
    }

  strqueue_sort(&sq);
  result = strqueue_join(&sq);
  strqueue_free(&sq);
  return result;
}


int
testcase_write(Solver *solv, char *dir, int withresult)
{
  Pool *pool = solv->pool;
  Repo *repo;
  int i;
  Id arch, repoid;
  Id lowscore;
  FILE *fp;
  Strqueue sq;
  char *cmd, *out;
  const char *s;

  if (mkdir(dir, 0777) && errno != EEXIST)
    {
      pool_debug(solv->pool, SOLV_ERROR, "testcase_write: could not create directory '%s'\n", dir);
      return 0;
    }
  strqueue_init(&sq);
  FOR_REPOS(repoid, repo)
    {
      const char *name = testcase_repoid2str(pool, repoid);
      char priobuf[50];
      if (repo->subpriority)
	sprintf(priobuf, "%d.%d", repo->priority, repo->subpriority);
      else
	sprintf(priobuf, "%d", repo->priority);
      out = pool_tmpjoin(pool, name, ".repo", ".gz");
      cmd = pool_tmpjoin(pool, "repo ", name, " ");
      cmd = pool_tmpappend(pool, cmd, priobuf, " ");
      cmd = pool_tmpappend(pool, cmd, "susetags ", out);
      strqueue_push(&sq, cmd);
      out = pool_tmpjoin(pool, dir, "/", out);
      if (!(fp = solv_xfopen(out, "w")))
	{
	  pool_debug(solv->pool, SOLV_ERROR, "testcase_write: could not open '%s' for writing\n", out);
	  strqueue_free(&sq);
	  return 0;
	}
      testcase_write_susetags(repo, fp);
      if (fclose(fp))
	{
	  pool_debug(solv->pool, SOLV_ERROR, "testcase_write: write error\n");
	  strqueue_free(&sq);
	  return 0;
	}
    }
  /* hmm, this is not optimal... we currently search for the lowest score */
  lowscore = 0;
  arch = ARCH_NOARCH;
  for (i = 0; i < pool->lastarch; i++)
    {
      if (pool->id2arch[i] == 1 && !lowscore)
	arch = i;
      if (pool->id2arch[i] > 0x10000 && (!lowscore || pool->id2arch[i] < lowscore))
	{
	  arch = i;
	  lowscore = pool->id2arch[i];
	}
    }
  cmd = pool_tmpjoin(pool, "system ", pool_id2str(pool, arch), pool->disttype == DISTTYPE_DEB ? " deb" : " rpm");
  if (pool->installed)
    cmd = pool_tmpappend(pool, cmd, " ", testcase_repoid2str(pool, pool->installed->repoid));
  strqueue_push(&sq, cmd);

  if (pool->vendorclasses)
    {
      cmd = 0;
      for (i = 0; pool->vendorclasses[i]; i++)
	{
	  cmd = pool_tmpappend(pool, cmd ? cmd : "vendorclass", " ", pool->vendorclasses[i]);
	  if (!pool->vendorclasses[i + 1])
	    {
	      strqueue_push(&sq, cmd);
	      cmd = 0;
	      i++;
	    }
	}
    }

  s = testcase_getsolverflags(solv);
  if (*s)
    {
      cmd = pool_tmpjoin(pool, "solverflags ", s, 0);
      strqueue_push(&sq, cmd);
    }

  /* now dump all the ns callback values we know */
  if (pool->nscallback)
    {
      Id rid;
      int d;
      for (rid = 1; rid < pool->nrels; rid++)
	{
	  Reldep *rd = pool->rels + rid;
	  if (rd->flags != REL_NAMESPACE || rd->name == NAMESPACE_OTHERPROVIDERS)
	    continue;
	  /* check if we evaluated it, also skip empty results */
	  if (!(d = pool->whatprovides_rel[rid]) || !pool->whatprovidesdata[d])
	    continue;
	  cmd = pool_tmpjoin(pool, "namespace ", pool_id2str(pool, rd->name), "(");
	  cmd = pool_tmpappend(pool, cmd, pool_id2str(pool, rd->evr), ")");
	  for (;  pool->whatprovidesdata[d]; d++)
	    cmd = pool_tmpappend(pool, cmd, " ", testcase_solvid2str(pool, pool->whatprovidesdata[d]));
	  strqueue_push(&sq, cmd);
	}
    }

  for (i = 0; i < solv->job.count; i += 2)
    {
      cmd = (char *)testcase_job2str(pool, solv->job.elements[i], solv->job.elements[i + 1]);
      cmd = pool_tmpjoin(pool, "job ", cmd, 0);
      strqueue_push(&sq, cmd);
    }

  if (withresult)
    {
      char *result;
      out = pool_tmpjoin(pool, dir, "/", "solver.result");
      if (!(fp = fopen(out, "w")))
	{
	  pool_debug(solv->pool, SOLV_ERROR, "testcase_write: could not open '%s' for writing\n", out);
	  strqueue_free(&sq);
	  return 0;
	}
      result = testcase_solverresult(solv);
      if (fwrite(result, strlen(result), 1, fp) != 1)
	{
	  pool_debug(solv->pool, SOLV_ERROR, "testcase_write: write error\n");
	  solv_free(result);
	  strqueue_free(&sq);
	  return 0;
	}
      solv_free(result);
      if (fclose(fp))
	{
	  pool_debug(solv->pool, SOLV_ERROR, "testcase_write: write error\n");
	  strqueue_free(&sq);
	  return 0;
	}
      cmd = pool_tmpjoin(pool, "result ", "solver.result", 0);
      strqueue_push(&sq, cmd);
    }

  cmd = strqueue_join(&sq);
  out = pool_tmpjoin(pool, dir, "/", "testcase.t");
  if (!(fp = fopen(out, "w")))
    {
      pool_debug(solv->pool, SOLV_ERROR, "testcase_write: could not open '%s' for writing\n", out);
      strqueue_free(&sq);
      return 0;
    }
  if (fwrite(cmd, strlen(cmd), 1, fp) != 1)
    {
      pool_debug(solv->pool, SOLV_ERROR, "testcase_write: write error\n");
      strqueue_free(&sq);
      return 0;
    }
  if (fclose(fp))
    {
      pool_debug(solv->pool, SOLV_ERROR, "testcase_write: write error\n");
      strqueue_free(&sq);
      return 0;
    }
  solv_free(cmd);
  strqueue_free(&sq);
  return 1;
}

Solver *
testcase_read(Pool *pool, FILE *fp, char *testcase, Queue *job, char **resultp)
{
  Solver *solv;
  char *buf, *bufp;
  int bufl;
  char *testcasedir, *s;
  int l;
  char **pieces = 0;
  int npieces = 0;
  int prepared = 0;
  int closefp = !fp;

  if (!fp && !(fp = fopen(testcase, "r")))
    {
      pool_debug(pool, SOLV_ERROR, "testcase_read: could not open '%s'\n", testcase);
      return 0;
    }
  testcasedir = solv_strdup(testcase);
  if ((s = strchr(testcasedir, '/')) != 0)
    s[1] = 0;
  else
    *testcasedir = 0;
  bufl = 1024;
  buf = solv_malloc(bufl);
  bufp = buf;
  solv = 0;
  for (;;)
    {
      if (bufp - buf + 16 > bufl)
	{
	  bufl = bufp - buf;
	  buf = solv_realloc(buf, bufl + 512);
	  bufp = buf + bufl;
          bufl += 512;
	}
      if (!fgets(bufp, bufl - (bufp - buf), fp))
	break;
      bufp = buf;
      l = strlen(buf);
      if (!l || buf[l - 1] != '\n')
	{
	  bufp += l;
	  continue;
	}
      buf[--l] = 0;
      s = buf;
      while (*s && (*s == ' ' || *s == '\t'))
	s++;
      if (!*s || *s == '#')
	continue;
      npieces = 0;
      /* split it in pieces */
      for (;;)
	{
	  while (*s == ' ' || *s == '\t')
	    s++;
	  if (!*s)
	    break;
	  pieces = solv_extend(pieces, npieces, 1, sizeof(*pieces), 7);
	  pieces[npieces++] = s;
	  while (*s && *s != ' ' && *s != '\t')
	    s++;
	  if (*s)
	    *s++ = 0;
	}
      pieces = solv_extend(pieces, npieces, 1, sizeof(*pieces), 7);
      pieces[npieces] = 0;
      if (!strcmp(pieces[0], "repo") && npieces >= 4)
	{
	  Repo *repo = repo_create(pool, pieces[1]);
	  FILE *rfp;
	  int prio, subprio;
	  const char *rdata;

	  prepared = 0;
	  if (sscanf(pieces[2], "%d.%d", &prio, &subprio) != 2)
	    {
	      subprio = 0;
	      prio = atoi(pieces[2]);
	    }
          repo->priority = prio;
          repo->subpriority = subprio;
	  rdata = pool_tmpjoin(pool, testcasedir, pieces[4], 0);
          if ((rfp = solv_xfopen(rdata, "r")) == 0)
	    {
	      pool_debug(pool, SOLV_ERROR, "testcase_read: could not open '%s'\n", rdata);
	    }
	  else if (!strcmp(pieces[3], "susetags"))
	    {
	      repo_add_susetags(repo, rfp, 0, 0, 0);
	      fclose(rfp);
	    }
	  else
	    {
	      fclose(rfp);
	      pool_debug(pool, SOLV_ERROR, "testcase_read: unknown repo type for repo '%s'\n", repo->name);
	    }
	}
      else if (!strcmp(pieces[0], "system") && npieces >= 3)
	{
	  prepared = 0;
	  pool_setarch(pool, pieces[1]);
	  if (npieces > 3)
	    {
	      Repo *repo = testcase_str2repo(pool, pieces[3]);
	      if (!repo)
	        pool_debug(pool, SOLV_ERROR, "testcase_read: system: unknown repo '%s'\n", pieces[3]);
	      else
		pool_set_installed(pool, repo);
	    }
	}
      else if (!strcmp(pieces[0], "job") && npieces > 1)
	{
	  char *sp;
	  Id how, what;
	  if (!prepared)
	    {
	      pool_addfileprovides(pool);
	      pool_createwhatprovides(pool);
	      prepared = 1;
	    }
	  /* rejoin */
	  for (sp = pieces[1]; sp < pieces[npieces - 1]; sp++)
	    if (*sp == 0)
	      *sp = ' ';
	  how = testcase_str2job(pool, pieces[1], &what);
	  if (how >= 0 && job)
	    queue_push2(job, how, what);
	}
      else if (!strcmp(pieces[0], "vendorclass") && npieces > 1)
	{
	  pool_addvendorclass(pool, (const char **)(pieces + 1));
	}
      else if (!strcmp(pieces[0], "namespace") && npieces > 1)
	{
	  int i = strlen(pieces[1]);
	  s = strchr(pieces[1], '(');
	  if (!s && pieces[1][i - 1] != ')')
	    {
	      pool_debug(pool, SOLV_ERROR, "testcase_read: bad namespace '%s'\n", pieces[1]);
	    }
	  else
	    {
	      Id name, evr, id;
	      Queue q;
	      queue_init(&q);
	      *s = 0;
	      pieces[1][i - 1] = 0;
	      name = pool_str2id(pool, pieces[1], 1);
	      evr = pool_str2id(pool, s + 1, 1);
	      *s = '(';
	      pieces[1][i - 1] = ')';
	      id = pool_rel2id(pool, name, evr, REL_NAMESPACE, 1);
	      for (i = 2; i < npieces; i++)
		queue_push(&q, testcase_str2solvid(pool, pieces[i]));
	      /* now do the callback */
	      if (!prepared)
		{
		  pool_addfileprovides(pool);
		  pool_createwhatprovides(pool);
		  prepared = 1;
		}
	      pool->whatprovides_rel[GETRELID(id)] = pool_queuetowhatprovides(pool, &q);
	      queue_free(&q);
	    }
	}
      else if (!strcmp(pieces[0], "solverflags") && npieces > 1)
        {
	  int i;
	  if (!solv)
	    {
	      solv = solver_create(pool);
	      testcase_resetsolverflags(solv);
	    }
	  for (i = 1; i < npieces; i++)
	    testcase_setsolverflags(solv, pieces[i]);
        }
      else if (!strcmp(pieces[0], "result") && npieces > 1)
	{
	  FILE *rfp;
	  const char *rdata;
	  rdata = pool_tmpjoin(pool, testcasedir, pieces[1], 0);
	  if (!strcmp(pieces[1], "<inline>"))
	    rfp = fp;
	  else
            rfp = fopen(rdata, "r");
          if (!rfp)
	    {
	      pool_debug(pool, SOLV_ERROR, "testcase_read: could not open '%s'\n", rdata);
	    }
	  else
	    {
	      /* slurp it in... */
	      char *result = solv_malloc(1024);
	      char *rp = result;
	      int resultl = 1024;
	      for (;;)
		{
		  size_t rl;
		  if (rp - result + 256 >= resultl)
		    {
		      resultl = rp - result;
		      result = solv_realloc(result, resultl + 1024);
		      rp = result + resultl;
		      resultl += 1024;
		    }
		  if (fp == rfp)
		    {
		      if (!fgets(rp, resultl - (rp - result), fp))
			rl = 0;
		      else
			{
		          rl = strlen(rp);
			  if (rl && (rp == result || rp[-1] == '\n'))
			    {
			      if (rl > 1 && rp[0] == '#' && rp[1] == '>')
				{
				  memmove(rp, rp + 2, rl - 2);
				  rl -= 2;
				}
			      else
				{
				  while (rl + 16 > bufl)
				    {
				      buf = solv_realloc(buf, bufl + 512);
				      bufl += 512;
				    }
				  memmove(buf, rp, rl);
				  if (buf[rl - 1] == '\n')
				    {
				      ungetc('\n', fp);
				      rl--;
				    }
				  bufp = buf + rl;
				  rl = 0;
				}
			    }
			}
		    }
		  else
		    rl = fread(rp, 1, resultl - (rp - result), rfp);
		  if (rl <= 0)
		    {
		      *rp = 0;
		      break;
		    }
		  rp += rl;
		}
	      if (rfp != fp)
	        fclose(rfp);
	      if (resultp)
		*resultp = result;
	      else
		solv_free(result);
	    }
	}
      else
	{
	  pool_debug(pool, SOLV_ERROR, "testcase_read: cannot parse command '%s'\n", pieces[0]);
	}
    }
  buf = solv_free(buf);
  pieces = solv_free(pieces);
  solv_free(testcasedir);
  if (!prepared)
    {
      pool_addfileprovides(pool);
      pool_createwhatprovides(pool);
    }
  if (!solv)
    {
      solv = solver_create(pool);
      testcase_resetsolverflags(solv);
    }
  if (closefp)
    fclose(fp);
  return solv;
}

char *
testcase_resultdiff(char *result1, char *result2)
{
  Strqueue sq1, sq2, osq;
  char *r;
  strqueue_init(&sq1);
  strqueue_init(&sq2);
  strqueue_init(&osq);
  strqueue_split(&sq1, result1);
  strqueue_split(&sq2, result2);
  strqueue_sort(&sq1);
  strqueue_sort(&sq2);
  strqueue_diff(&sq1, &sq2, &osq);
  r = osq.nstr ? strqueue_join(&osq) : 0;
  strqueue_free(&sq1);
  strqueue_free(&sq2);
  strqueue_free(&osq);
  return r;
}

