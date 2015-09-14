#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "pool.h"
#include "repo.h"
#ifdef ENABLE_PUBKEY
#include "repo_pubkey.h"
#endif

#include "checksig.h"

#ifndef DEBIAN

static void
cleanupgpg(char *gpgdir)
{
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "%s/pubring.gpg", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/pubring.gpg~", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/secring.gpg", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/trustdb.gpg", gpgdir);
  unlink(cmd);
  snprintf(cmd, sizeof(cmd), "%s/keys", gpgdir);
  unlink(cmd);
  rmdir(gpgdir);
}

int
checksig(Pool *sigpool, FILE *fp, FILE *sigfp)
{
  char *gpgdir;
  char *keysfile;
  const char *pubkey;
  char cmd[256];
  FILE *kfp;
  Solvable *s;
  Id p;
  off_t posfp, possigfp;
  int r, nkeys;

  gpgdir = mkdtemp(pool_tmpjoin(sigpool, "/var/tmp/solvgpg.XXXXXX", 0, 0));
  if (!gpgdir)
    return 0;
  keysfile = pool_tmpjoin(sigpool, gpgdir, "/keys", 0);
  if (!(kfp = fopen(keysfile, "w")) )
    {
      cleanupgpg(gpgdir);
      return 0;
    }
  nkeys = 0;
  for (p = 1, s = sigpool->solvables + p; p < sigpool->nsolvables; p++, s++)
    {
      if (!s->repo)
	continue;
      pubkey = solvable_lookup_str(s, SOLVABLE_DESCRIPTION);
      if (!pubkey || !*pubkey)
	continue;
      if (fwrite(pubkey, strlen(pubkey), 1, kfp) != 1)
	break;
      if (fputc('\n', kfp) == EOF)	/* Just in case... */
	break;
      nkeys++;
    }
  if (fclose(kfp) || !nkeys || p < sigpool->nsolvables)
    {
      cleanupgpg(gpgdir);
      return 0;
    }
  snprintf(cmd, sizeof(cmd), "gpg2 -q --homedir %s --import %s", gpgdir, keysfile);
  if (system(cmd))
    {
      fprintf(stderr, "key import error\n");
      cleanupgpg(gpgdir);
      return 0;
    }
  unlink(keysfile);
  posfp = lseek(fileno(fp), 0, SEEK_CUR);
  lseek(fileno(fp), 0, SEEK_SET);
  possigfp = lseek(fileno(sigfp), 0, SEEK_CUR);
  lseek(fileno(sigfp), 0, SEEK_SET);
  snprintf(cmd, sizeof(cmd), "gpgv -q --homedir %s --keyring %s/pubring.gpg /dev/fd/%d /dev/fd/%d >/dev/null 2>&1", gpgdir, gpgdir, fileno(sigfp), fileno(fp));
  fcntl(fileno(fp), F_SETFD, 0);	/* clear CLOEXEC */
  fcntl(fileno(sigfp), F_SETFD, 0);	/* clear CLOEXEC */
  r = system(cmd);
  lseek(fileno(sigfp), possigfp, SEEK_SET);
  lseek(fileno(fp), posfp, SEEK_SET);
  fcntl(fileno(fp), F_SETFD, FD_CLOEXEC);
  fcntl(fileno(sigfp), F_SETFD, FD_CLOEXEC);
  cleanupgpg(gpgdir);
  return r == 0 ? 1 : 0;
}

#else

int
checksig(Pool *sigpool, FILE *fp, FILE *sigfp)
{
  char cmd[256];
  int r;

  snprintf(cmd, sizeof(cmd), "gpgv -q --keyring /etc/apt/trusted.gpg /dev/fd/%d /dev/fd/%d >/dev/null 2>&1", fileno(sigfp), fileno(fp));
  fcntl(fileno(fp), F_SETFD, 0);	/* clear CLOEXEC */
  fcntl(fileno(sigfp), F_SETFD, 0);	/* clear CLOEXEC */
  r = system(cmd);
  fcntl(fileno(fp), F_SETFD, FD_CLOEXEC);
  fcntl(fileno(sigfp), F_SETFD, FD_CLOEXEC);
  return r == 0 ? 1 : 0;
}

#endif

Pool *
read_sigs()
{
  Pool *sigpool = pool_create();
#if defined(ENABLE_PUBKEY) && defined(ENABLE_RPMDB)
  Repo *repo = repo_create(sigpool, "pubkeys");
  repo_add_rpmdb_pubkeys(repo, 0);
#endif
  return sigpool;
}
