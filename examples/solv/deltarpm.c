#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "pool.h"
#include "repo.h"
#include "repoinfo.h"
#include "repoinfo_download.h"

#include "deltarpm.h"

static inline int
opentmpfile()
{
  char tmpl[100];
  int fd;

  strcpy(tmpl, "/var/tmp/solvXXXXXX");
  fd = mkstemp(tmpl);
  if (fd < 0) 
    {    
      perror("mkstemp");
      exit(1);
    }    
  unlink(tmpl);
  return fd;
}

FILE *
trydeltadownload(Solvable *s, const char *loc)
{
  Repo *repo = s->repo;
  Pool *pool = repo->pool;
  struct repoinfo *cinfo = repo->appdata;
  Dataiterator di;
  Id pp;
  const unsigned char *chksum;
  Id chksumtype;
  FILE *retfp = 0;
  char *matchname = strdup(pool_id2str(pool, s->name));

  dataiterator_init(&di, pool, repo, SOLVID_META, DELTA_PACKAGE_NAME, matchname, SEARCH_STRING);
  dataiterator_prepend_keyname(&di, REPOSITORY_DELTAINFO);
  while (dataiterator_step(&di))
    {
      Id baseevr, op;

      dataiterator_setpos_parent(&di);
      if (pool_lookup_id(pool, SOLVID_POS, DELTA_PACKAGE_EVR) != s->evr ||
	  pool_lookup_id(pool, SOLVID_POS, DELTA_PACKAGE_ARCH) != s->arch)
	continue;
      baseevr = pool_lookup_id(pool, SOLVID_POS, DELTA_BASE_EVR);
      FOR_PROVIDES(op, pp, s->name)
	{
	  Solvable *os = pool->solvables + op;
	  if (os->repo == pool->installed && os->name == s->name && os->arch == s->arch && os->evr == baseevr)
	    break;
	}
      if (op && access("/usr/bin/applydeltarpm", X_OK) == 0)
	{
	  /* base is installed, run sequence check */
	  const char *seq;
	  const char *dloc;
	  const char *archstr;
	  FILE *fp;
	  char cmd[128];
	  int newfd;

	  archstr = pool_id2str(pool, s->arch);
	  if (strlen(archstr) > 10 || strchr(archstr, '\'') != 0)
	    continue;

	  seq = pool_tmpjoin(pool, pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_NAME), "-", pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_EVR));
	  seq = pool_tmpappend(pool, seq, "-", pool_lookup_str(pool, SOLVID_POS, DELTA_SEQ_NUM));
	  if (strchr(seq, '\'') != 0)
	    continue;
#if defined(FEDORA) || defined(MAGEIA)
	  sprintf(cmd, "/usr/bin/applydeltarpm -a '%s' -c -s '", archstr);
#else
	  sprintf(cmd, "/usr/bin/applydeltarpm -c -s '");
#endif
	  if (system(pool_tmpjoin(pool, cmd, seq, "'")) != 0)
	    continue;	/* didn't match */
	  /* looks good, download delta */
	  chksumtype = 0;
	  chksum = pool_lookup_bin_checksum(pool, SOLVID_POS, DELTA_CHECKSUM, &chksumtype);
	  if (!chksumtype)
	    continue;	/* no way! */
	  dloc = pool_lookup_deltalocation(pool, SOLVID_POS, 0);
	  if (!dloc)
	    continue;
#ifdef ENABLE_SUSEREPO
	  if (cinfo->type == TYPE_SUSETAGS)
	    {
	      const char *datadir = repo_lookup_str(repo, SOLVID_META, SUSETAGS_DATADIR);
	      dloc = pool_tmpjoin(pool, datadir ? datadir : "suse", "/", dloc);
	    }
#endif
	  if ((fp = curlfopen(cinfo, dloc, 0, chksum, chksumtype, 0)) == 0)
	    continue;
	  /* got it, now reconstruct */
	  newfd = opentmpfile();
#if defined(FEDORA) || defined(MAGEIA)
	  sprintf(cmd, "applydeltarpm -a '%s' /dev/fd/%d /dev/fd/%d", archstr, fileno(fp), newfd);
#else
	  sprintf(cmd, "applydeltarpm /dev/fd/%d /dev/fd/%d", fileno(fp), newfd);
#endif
	  fcntl(fileno(fp), F_SETFD, 0);
	  if (system(cmd))
	    {
	      close(newfd);
	      fclose(fp);
	      continue;
	    }
	  lseek(newfd, 0, SEEK_SET);
	  chksumtype = 0;
	  chksum = solvable_lookup_bin_checksum(s, SOLVABLE_CHECKSUM, &chksumtype);
	  if (chksumtype && !verify_checksum(newfd, loc, chksum, chksumtype))
	    {
	      close(newfd);
	      fclose(fp);
	      continue;
	    }
	  retfp = fdopen(newfd, "r");
	  fclose(fp);
	  break;
	}
    }
  dataiterator_free(&di);
  solv_free(matchname);
  return retfp;
}
