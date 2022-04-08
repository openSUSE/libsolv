/*
 * Copyright (c) 2019, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "pool.h"
#include "repo.h"
#include "testcase.h"

#define DISABLE_JOIN2
#include "tools_util.h"

static const char *
testcase_id2str(Pool *pool, Id id, int isname)
{
  const char *s = pool_id2str(pool, id);
  const char *ss;
  char *s2, *s2p;
  int bad = 0, paren = 0, parenbad = 0;

  if (id == 0)
    return "<NULL>";
  if (id == 1)
    return "\\00";
  if (strchr("[(<=>!", *s))
    bad++;
  if (!strncmp(s, "namespace:", 10))
    bad++;
  for (ss = s + bad; *ss; ss++)
    {
      if (*ss == ' ' || *ss == '\\' || *(unsigned char *)ss < 32 || *ss == '(' || *ss == ')')
        bad++;
      if (*ss == '(')
	paren = paren == 0 ? 1 : -1;
      else if (*ss == ')')
	{
	  paren = paren == 1 ? 0 : -1;
	  if (!paren)
	    parenbad += 2;
	}
    }
  if (isname && ss - s > 4 && !strcmp(ss - 4, ":any"))
    bad++;
  if (!paren && !(bad - parenbad))
    return s;

  /* we need escaping! */
  s2 = s2p = pool_alloctmpspace(pool, strlen(s) + bad * 2 + 1);
  if (!strncmp(s, "namespace:", 10))
    {
      strcpy(s2p, "namespace\\3a");
      s2p += 12;
      s += 10;
    }
  ss = s;
  for (; *ss; ss++)
    {
      *s2p++ = *ss;
      if ((ss == s && strchr("[(<=>!", *s)) || *ss == ' ' || *ss == '\\' || *(unsigned char *)ss < 32 || *ss == '(' || *ss == ')')
	{
	  s2p[-1] = '\\';
	  solv_bin2hex((unsigned char *)ss, 1, s2p);
	  s2p += 2;
	}
    }
  *s2p = 0;
  if (isname && s2p - s2 > 4 && !strcmp(s2p - 4, ":any"))
    strcpy(s2p - 4, "\\3aany");
  return s2;
}

struct oplist {
  Id flags;
  const char *opname;
} oplist[] = {
  { REL_EQ, "=" },
  { REL_GT | REL_LT | REL_EQ, "<=>" },
  { REL_LT | REL_EQ, "<=" },
  { REL_GT | REL_EQ, ">=" },
  { REL_GT, ">" },
  { REL_GT | REL_LT, "<>" },
  { REL_AND,   "&" },
  { REL_OR ,   "|" },
  { REL_WITH , "+" },
  { REL_WITHOUT , "-" },
  { REL_NAMESPACE , "<NAMESPACE>" },
  { REL_ARCH,       "." },
  { REL_MULTIARCH,  "<MULTIARCH>" },
  { REL_FILECONFLICT,  "<FILECONFLICT>" },
  { REL_COND,  "<IF>" },
  { REL_COMPAT,  "compat >=" },
  { REL_KIND,  "<KIND>" },
  { REL_ELSE, "<ELSE>" },
  { REL_ERROR, "<ERROR>" },
  { REL_UNLESS, "<UNLESS>" },
  { REL_CONDA, "<CONDA>" },
  { REL_LT, "<" },
  { 0, 0 }
};

static char *
testcase_dep2str_complex(Pool *pool, char *s, Id id, int addparens)
{
  Reldep *rd;
  const char *s2;
  int needparens;
  struct oplist *op;

  if (!ISRELDEP(id))
    {
      s2 = testcase_id2str(pool, id, 1);
      s = pool_tmpappend(pool, s, s2, 0);
      pool_freetmpspace(pool, s2);
      return s;
    }
  rd = GETRELDEP(pool, id);

  /* check for special shortcuts */
  if (rd->flags == REL_NAMESPACE && !ISRELDEP(rd->name) && !strncmp(pool_id2str(pool, rd->name), "namespace:", 10))
    {
      s = pool_tmpappend(pool, s, pool_id2str(pool, rd->name), "(");
      s = testcase_dep2str_complex(pool, s, rd->evr, 0);
      return pool_tmpappend(pool, s, ")", 0);
    }
  if (rd->flags == REL_MULTIARCH && !ISRELDEP(rd->name) && rd->evr == ARCH_ANY)
    {
      /* append special :any suffix */
      s2 = testcase_id2str(pool, rd->name, 1);
      s = pool_tmpappend(pool, s, s2, ":any");
      pool_freetmpspace(pool, s2);
      return s;
    }

  needparens = 0;
  if (ISRELDEP(rd->name))
    {
      Reldep *rd2 = GETRELDEP(pool, rd->name);
      needparens = 1;
      if (rd->flags > 7 && rd->flags != REL_COMPAT && rd2->flags && rd2->flags <= 7)
	needparens = 0;
    }

  if (addparens)
    s = pool_tmpappend(pool, s, "(", 0);
  s = testcase_dep2str_complex(pool, s, rd->name, needparens);

  for (op = oplist; op->flags; op++)
    if (rd->flags == op->flags)
      break;
  if (op->flags)
    {
      s = pool_tmpappend(pool, s, " ", op->opname);
      s = pool_tmpappend(pool, s, " ", 0);
    }
  else
    {
      char buf[64];
      sprintf(buf, " <%u> ", rd->flags);
      s = pool_tmpappend(pool, s, buf, 0);
    }

  needparens = 0;
  if (ISRELDEP(rd->evr))
    {
      Reldep *rd2 = GETRELDEP(pool, rd->evr);
      needparens = 1;
      if (rd->flags > 7 && rd2->flags && rd2->flags <= 7)
	needparens = 0;
      if (rd->flags == REL_AND && rd2->flags == REL_AND)
	needparens = 0;	/* chain */
      if (rd->flags == REL_OR && rd2->flags == REL_OR)
	needparens = 0;	/* chain */
      if (rd->flags > 0 && rd->flags < 8 && rd2->flags == REL_COMPAT)
	needparens = 0;	/* chain */
    }
  if (!ISRELDEP(rd->evr))
    {
      s2 = testcase_id2str(pool, rd->evr, 0);
      s = pool_tmpappend(pool, s, s2, 0);
      pool_freetmpspace(pool, s2);
    }
  else
    s = (char *)testcase_dep2str_complex(pool, s, rd->evr, needparens);
  if (addparens)
    s = pool_tmpappend(pool, s, ")", 0);
  return s;
}

const char *
testcase_dep2str(Pool *pool, Id id)
{
  char *s;
  if (!ISRELDEP(id))
    return testcase_id2str(pool, id, 1);
  s = pool_alloctmpspace(pool, 1);
  *s = 0;
  return testcase_dep2str_complex(pool, s, id, 0);
}


/* Convert a simple string. Also handle the :any suffix */
static Id
testcase_str2dep_simple(Pool *pool, const char **sp, int isname)
{
  int haveesc = 0;
  int paren = 0;
  int isany = 0;
  Id id;
  const char *s;
  for (s = *sp; *s; s++)
    {
      if (*s == '\\')
	haveesc++;
      if (*s == ' ' || *(unsigned char *)s < 32)
	break;
      if (*s == '(')
	paren++;
      if (*s == ')' && paren-- <= 0)
	break;
    }
  if (isname && s - *sp > 4 && !strncmp(s - 4, ":any", 4))
    {
      isany = 1;
      s -= 4;
    }
  if (!haveesc)
    {
      if (s - *sp == 6 && !strncmp(*sp, "<NULL>", 6))
	id = 0;
      else
        id = pool_strn2id(pool, *sp, s - *sp, 1);
    }
  else if (s - *sp == 3 && !strncmp(*sp, "\\00", 3))
    id = 1;
  else
    {
      char buf[128], *bp, *bp2;
      const char *sp2;
      bp = s - *sp >= 128 ? solv_malloc(s - *sp + 1) : buf;
      for (bp2 = bp, sp2 = *sp; sp2 < s;)
	{
	  *bp2++ = *sp2++;
	  if (bp2[-1] == '\\')
	    solv_hex2bin(&sp2, (unsigned char *)bp2 - 1, 1);
	}
      *bp2 = 0;
      id = pool_str2id(pool, bp, 1);
      if (bp != buf)
	solv_free(bp);
    }
  if (isany)
    {
      id = pool_rel2id(pool, id, ARCH_ANY, REL_MULTIARCH, 1);
      s += 4;
    }
  *sp = s;
  return id;
}


static Id
testcase_str2dep_complex(Pool *pool, const char **sp, int relop)
{
  const char *s = *sp;
  Id flags, id, id2, namespaceid = 0;
  struct oplist *op;

  if (!s)
    return 0;
  while (*s == ' ' || *s == '\t')
    s++;
  if (!strncmp(s, "namespace:", 10))
    {
      /* special namespace hack */
      const char *s2;
      for (s2 = s + 10; *s2 && *s2 != '('; s2++)
	;
      if (*s2 == '(')
	{
	  namespaceid = pool_strn2id(pool, s, s2 - s, 1);
	  s = s2;
	}
    }
  if (*s == '(')
    {
      s++;
      id = testcase_str2dep_complex(pool, &s, 0);
      if (!s || *s != ')')
	{
	  *sp = 0;
	  return 0;
	}
      s++;
    }
  else
    id = testcase_str2dep_simple(pool, &s, relop ? 0 : 1);
  if (namespaceid)
    id = pool_rel2id(pool, namespaceid, id, REL_NAMESPACE, 1);
    
  for (;;)
    {
      while (*s == ' ' || *s == '\t')
	s++;
      if (!*s || *s == ')' || (relop && strncmp(s, "compat >= ", 10) != 0))
	{
	  *sp = s;
	  return id;
	}

      /* we have an op! Find the end */
      flags = -1;
      if (s[0] == '<' && (s[1] >= '0' && s[1] <= '9'))
	{
	  const char *s2;
	  for (s2 = s + 1; *s2 >= '0' && *s2 <= '9'; s2++)
	    ;
	  if (*s2 == '>')
	    {
	      flags = strtoul(s + 1, 0, 10);
	      s = s2 + 1;
	    }
	}
      if (flags == -1)
	{
	  for (op = oplist; op->flags; op++)
	    if (!strncmp(s, op->opname, strlen(op->opname)))
	      break;
	  if (!op->flags)
	    {
	      *sp = 0;
	      return 0;
	    }
	  flags = op->flags;
	  s += strlen(op->opname);
	}
      id2 = testcase_str2dep_complex(pool, &s, flags > 0 && flags < 8);
      if (!s)
	{
	  *sp = 0;
	  return 0;
	}
      id = pool_rel2id(pool, id, id2, flags, 1);
    }
}

Id
testcase_str2dep(Pool *pool, const char *s)
{
  Id id = testcase_str2dep_complex(pool, &s, 0);
  return s && !*s ? id : 0;
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
	continue;
      idstr = testcase_dep2str(pool, id);
      if (!tagwritten)
	{
	  fprintf(fp, "+%s\n", tag);
	  tagwritten = 1;
	}
      if (*idstr == '/' && !ISRELDEP(id)) {
        fprintf(fp, "%s\n", pool_id2str(pool, id));
      } else if (*idstr == '/') {
        fprintf(fp, "\\2f%s\n", idstr + 1);
      } else {
        fprintf(fp, "%s\n", idstr);
      }
    }
  if (tagwritten)
    fprintf(fp, "-%s\n", tag);
}

static void
writefilelist(Repo *repo, FILE *fp, const char *tag, Solvable *s)
{
  Pool *pool = repo->pool;
  int tagwritten = 0;
  Dataiterator di;

  dataiterator_init(&di, pool, repo, s - pool->solvables, SOLVABLE_FILELIST, 0, 0);
  while (dataiterator_step(&di))
    {
      const char *s = repodata_dir2str(di.data, di.kv.id, di.kv.str);
      if (!tagwritten)
	{
	  fprintf(fp, "+%s\n", tag);
	  tagwritten = 1;
	}
      fprintf(fp, "%s\n", s);
    }
  if (tagwritten)
    fprintf(fp, "-%s\n", tag);
  dataiterator_free(&di);
}

int
testcase_write_testtags(Repo *repo, FILE *fp)
{
  Pool *pool = repo->pool;
  Solvable *s;
  Id p;
  const char *name;
  const char *evr;
  const char *arch;
  const char *release;
  const char *tmp;
  unsigned int ti;
  Queue q;

  fprintf(fp, "=Ver: 3.0\n");
  queue_init(&q);
  FOR_REPO_SOLVABLES(repo, p, s)
    {
      name = pool_id2str(pool, s->name);
      evr = pool_id2str(pool, s->evr);
      arch = s->arch ? pool_id2str(pool, s->arch) : "-";
      release = strrchr(evr, '-');
      if (!release)
	release = evr + strlen(evr);
      fprintf(fp, "=Pkg: %s %.*s %s %s\n", name, (int)(release - evr), evr, *release && release[1] ? release + 1 : "-", arch);
      tmp = solvable_lookup_str(s, SOLVABLE_SUMMARY);
      if (tmp)
        fprintf(fp, "=Sum: %s\n", tmp);
      writedeps(repo, fp, "Req:", SOLVABLE_REQUIRES, s, s->requires);
      writedeps(repo, fp, "Prv:", SOLVABLE_PROVIDES, s, s->provides);
      writedeps(repo, fp, "Obs:", SOLVABLE_OBSOLETES, s, s->obsoletes);
      writedeps(repo, fp, "Con:", SOLVABLE_CONFLICTS, s, s->conflicts);
      writedeps(repo, fp, "Rec:", SOLVABLE_RECOMMENDS, s, s->recommends);
      writedeps(repo, fp, "Sup:", SOLVABLE_SUPPLEMENTS, s, s->supplements);
      writedeps(repo, fp, "Sug:", SOLVABLE_SUGGESTS, s, s->suggests);
      writedeps(repo, fp, "Enh:", SOLVABLE_ENHANCES, s, s->enhances);
      if (solvable_lookup_idarray(s, SOLVABLE_PREREQ_IGNOREINST, &q))
	{
	  int i;
	  fprintf(fp, "+Ipr:\n");
	  for (i = 0; i < q.count; i++)
	    fprintf(fp, "%s\n", testcase_dep2str(pool, q.elements[i]));
	  fprintf(fp, "-Ipr:\n");
	}
      if (solvable_lookup_idarray(s, SOLVABLE_CONSTRAINS, &q))
	{
	  int i;
	  fprintf(fp, "+Cns:\n");
	  for (i = 0; i < q.count; i++)
	    fprintf(fp, "%s\n", testcase_dep2str(pool, q.elements[i]));
	  fprintf(fp, "-Cns:\n");
	}
      if (s->vendor)
	fprintf(fp, "=Vnd: %s\n", pool_id2str(pool, s->vendor));
      if (solvable_lookup_idarray(s, SOLVABLE_BUILDFLAVOR, &q))
	{
	  int i;
	  for (i = 0; i < q.count; i++)
	    fprintf(fp, "=Flv: %s\n", pool_id2str(pool, q.elements[i]));
	}
      tmp = solvable_lookup_str(s, SOLVABLE_BUILDVERSION);
      if (tmp)
        fprintf(fp, "=Bvr: %s\n", tmp);
      if (solvable_lookup_idarray(s, SOLVABLE_TRACK_FEATURES, &q))
	{
	  int i;
	  for (i = 0; i < q.count; i++)
	    fprintf(fp, "=Trf: %s\n", pool_id2str(pool, q.elements[i]));
	}
      ti = solvable_lookup_num(s, SOLVABLE_BUILDTIME, 0);
      if (ti)
	fprintf(fp, "=Tim: %u\n", ti);
      ti = solvable_lookup_num(s, SOLVABLE_INSTALLTIME, 0);
      if (ti)
	fprintf(fp, "=Itm: %u\n", ti);
      writefilelist(repo, fp, "Fls:", s);
    }
  queue_free(&q);
  return 0;
}

static inline Offset
adddep(Repo *repo, Offset olddeps, char *str, Id marker)
{
  Id id = *str == '/' ? pool_str2id(repo->pool, str, 1) : testcase_str2dep(repo->pool, str);
  return repo_addid_dep(repo, olddeps, id, marker);
}

static void
finish_v2_solvable(Pool *pool, Repodata *data, Solvable *s, char *filelist, int nfilelist)
{
  if (nfilelist)
    {
      int l;
      Id did;
      for (l = 0; l < nfilelist; l += strlen(filelist + l) + 1)
	{
	  char *p = strrchr(filelist + l, '/');
	  if (!p)
	    continue;
	  *p++ = 0;
	  did = repodata_str2dir(data, filelist + l, 1);
	  p[-1] = '/';
	  if (!did)
	    did = repodata_str2dir(data, "/", 1);
	  repodata_add_dirstr(data, s - pool->solvables, SOLVABLE_FILELIST, did, p);
	}
    }
  repo_rewrite_suse_deps(s, 0);
}

/* stripped down version of susetags parser used for testcases */
int
testcase_add_testtags(Repo *repo, FILE *fp, int flags)
{
  Pool *pool = repo->pool;
  char *line, *linep;
  int aline;
  int tag;
  Repodata *data;
  Solvable *s;
  char *sp[5];
  unsigned int t;
  int intag;
  char *filelist = 0;
  int afilelist = 0;
  int nfilelist = 0;
  int tagsversion = 0;
  int addselfprovides = 1;	/* for compat reasons */

  data = repo_add_repodata(repo, flags);
  s = 0;
  intag = 0;

  aline = 1024;
  line = solv_malloc(aline);
  linep = line;
  for (;;)
    {
      if (linep - line + 16 > aline)
	{
	  aline = linep - line;
	  line = solv_realloc(line, aline + 512);
	  linep = line + aline;
	  aline += 512;
	}
      if (!fgets(linep, aline - (linep - line), fp))
	break;
      linep += strlen(linep);
      if (linep == line || linep[-1] != '\n')
	continue;
      linep[-1] = 0;
      linep = line + intag;
      if (intag)
	{
	  if (line[intag] == '-' && !strncmp(line + 1, line + intag + 1, intag - 2))
	    {
	      intag = 0;
	      linep = line;
	      continue;
	    }
	}
      else if (line[0] == '+' && line[1] && line[1] != ':')
	{
	  char *tagend = strchr(line, ':');
	  if (!tagend)
	    continue;
	  line[0] = '=';
	  tagend[1] = ' ';
	  intag = tagend + 2 - line;
	  linep = line + intag;
	  continue;
	}
      if (*line != '=' || !line[1] || !line[2] || !line[3] || line[4] != ':')
	continue;
      tag = line[1] << 16 | line[2] << 8 | line[3];
      /* tags that do not need a solvable */
      switch(tag)
	{
	case 'V' << 16 | 'e' << 8 | 'r':
	  tagsversion = atoi(line + 6);
	  addselfprovides = tagsversion < 3 || strstr(line + 6, "addselfprovides") != 0;
	  continue;
	case 'P' << 16 | 'k' << 8 | 'g':
	  if (s)
	    {
	      if (tagsversion == 2)
		finish_v2_solvable(pool, data, s, filelist, nfilelist);
	      if (addselfprovides && s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
		s->provides = repo_addid_dep(s->repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
	    }
	  nfilelist = 0;
	  if (split(line + 5, sp, 5) != 4)
	    break;
	  s = pool_id2solvable(pool, repo_add_solvable(repo));
	  s->name = pool_str2id(pool, sp[0], 1);
	  /* join back version and release */
	  if (sp[2] && !(sp[2][0] == '-' && !sp[2][1]))
	    sp[2][-1] = '-';
	  s->evr = makeevr(pool, sp[1]);
	  s->arch = strcmp(sp[3], "-") ? pool_str2id(pool, sp[3], 1) : 0;
	  continue;
	default:
	  break;
	}
      if (!s)
	continue;
      /* tags that need a solvable */
      switch(tag)
	{
	case 'S' << 16 | 'u' << 8 | 'm':
	  repodata_set_str(data, s - pool->solvables, SOLVABLE_SUMMARY, line + 6);
	  break;
	case 'V' << 16 | 'n' << 8 | 'd':
	  s->vendor = pool_str2id(pool, line + 6, 1);
	  break;
	case 'T' << 16 | 'i' << 8 | 'm':
	  t = atoi(line + 6);
	  if (t)
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_BUILDTIME, t);
	  break;
	case 'I' << 16 | 't' << 8 | 'm':
	  t = atoi(line + 6);
	  if (t)
	    repodata_set_num(data, s - pool->solvables, SOLVABLE_INSTALLTIME, t);
	  break;
	case 'R' << 16 | 'e' << 8 | 'q':
	  s->requires = adddep(repo, s->requires, line + 6, -SOLVABLE_PREREQMARKER);
	  break;
	case 'P' << 16 | 'r' << 8 | 'q':
	  s->requires = adddep(repo, s->requires, line + 6, SOLVABLE_PREREQMARKER);
	  break;
	case 'P' << 16 | 'r' << 8 | 'v':
	  /* version 2 had the file list at the end of the provides */
	  if (tagsversion == 2)
	    {
	      if (line[6] == '/')
		{
		  int l = strlen(line + 6) + 1;
		  if (nfilelist + l > afilelist)
		    {
		      afilelist = nfilelist + l + 512;
		      filelist = solv_realloc(filelist, afilelist);
		    }
		  memcpy(filelist + nfilelist, line + 6, l);
		  nfilelist += l;
		  break;
		}
	      if (nfilelist)
		{
		  int l;
		  for (l = 0; l < nfilelist; l += strlen(filelist + l) + 1)
		    s->provides = repo_addid_dep(repo, s->provides, pool_str2id(pool, filelist + l, 1), 0);
		  nfilelist = 0;
		}
	    }
	  s->provides = adddep(repo, s->provides, line + 6, 0);
	  break;
	case 'F' << 16 | 'l' << 8 | 's':
	  {
	    char *p = strrchr(line + 6, '/');
	    Id did;
	    if (!p)
	      break;
	    *p++ = 0;
	    did = repodata_str2dir(data, line + 6, 1);
	    if (!did)
	      did = repodata_str2dir(data, "/", 1);
	    repodata_add_dirstr(data, s - pool->solvables, SOLVABLE_FILELIST, did, p);
	    break;
	  }
	case 'O' << 16 | 'b' << 8 | 's':
	  s->obsoletes = adddep(repo, s->obsoletes, line + 6, 0);
	  break;
	case 'C' << 16 | 'o' << 8 | 'n':
	  s->conflicts = adddep(repo, s->conflicts, line + 6, 0);
	  break;
	case 'R' << 16 | 'e' << 8 | 'c':
	  s->recommends = adddep(repo, s->recommends, line + 6, 0);
	  break;
	case 'S' << 16 | 'u' << 8 | 'p':
	  s->supplements = adddep(repo, s->supplements, line + 6, 0);
	  break;
	case 'S' << 16 | 'u' << 8 | 'g':
	  s->suggests = adddep(repo, s->suggests, line + 6, 0);
	  break;
	case 'E' << 16 | 'n' << 8 | 'h':
	  s->enhances = adddep(repo, s->enhances, line + 6, 0);
	  break;
	case 'I' << 16 | 'p' << 8 | 'r':
	  {
	    Id id = line[6] == '/' ? pool_str2id(pool, line + 6, 1) : testcase_str2dep(pool, line + 6);
	    repodata_add_idarray(data, s - pool->solvables, SOLVABLE_PREREQ_IGNOREINST, id);
	    break;
	  }
	case 'C' << 16 | 'n' << 8 | 's':
	  repodata_add_idarray(data, s - pool->solvables, SOLVABLE_CONSTRAINS, testcase_str2dep(pool, line + 6));
	  break;
	case 'F' << 16 | 'l' << 8 | 'v':
	  repodata_add_poolstr_array(data, s - pool->solvables, SOLVABLE_BUILDFLAVOR, line + 6);
	  break;
	case 'B' << 16 | 'v' << 8 | 'r':
	  repodata_set_str(data, s - pool->solvables, SOLVABLE_BUILDVERSION, line + 6);
	  break;
	case 'T' << 16 | 'r' << 8 | 'f':
	  repodata_add_poolstr_array(data, s - pool->solvables, SOLVABLE_TRACK_FEATURES, line + 6);
	  break;
        default:
	  break;
        }
    }
  if (s)
    {
      if (tagsversion == 2)
	finish_v2_solvable(pool, data, s, filelist, nfilelist);
      if (addselfprovides && s->name && s->arch != ARCH_SRC && s->arch != ARCH_NOSRC)
        s->provides = repo_addid_dep(s->repo, s->provides, pool_rel2id(pool, s->name, s->evr, REL_EQ, 1), 0);
    }
  solv_free(line);
  solv_free(filelist);
  repodata_free_dircache(data);
  if (!(flags & REPO_NO_INTERNALIZE))
    repodata_internalize(data);
  return 0;
}
