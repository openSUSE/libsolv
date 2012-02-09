/*
 * Copyright (c) 2012, Novell Inc.
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

#include "pool.h"
#include "repo.h"
#include "solver.h"
#include "testcase.h"


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
  Id flags;
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
testcase_solvid2str(Pool *pool, Id p)
{
  Solvable *s = pool->solvables + p;
  const char *str = pool_solvid2str(pool, p);
  char buf[20];

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
    if ((how & jobflags2str[i].flags) != 0)
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
	      job |= jobflags2str[i].flags;
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
  Id id, *dp;
  int tagwritten = 0;
  const char *idstr;

  if (!off)
    return;
  dp = repo->idarraydata + off;
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
	break;
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
	  if (!tagwritten)
	    {
	      fprintf(fp, "+%s", tag);
	      tagwritten = 1;
	    }
	  fprintf(fp, "%s\n", repodata_dir2str(di.data, di.kv.id, di.kv.str));
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
