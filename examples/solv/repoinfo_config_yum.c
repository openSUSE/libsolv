#if defined(SUSE) || defined(FEDORA) || defined(MAGEIA)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/utsname.h>

#include "pool.h"
#include "repo.h"
#include "repo_rpmdb.h"

#include "repoinfo.h"
#include "repoinfo_config_yum.h"


#if defined(FEDORA) || defined(MAGEIA)
# define REPOINFO_PATH "/etc/yum.repos.d"
#endif
#ifdef SUSE
# define REPOINFO_PATH "/etc/zypp/repos.d"
#endif

char *
yum_substitute(Pool *pool, char *line)
{
  char *p, *p2;
  static char *releaseevr;
  static char *basearch;

  if (!line)
    {
      solv_free(releaseevr);
      releaseevr = 0;
      solv_free(basearch);
      basearch = 0;
      return 0;
    }
  p = line;
  while ((p2 = strchr(p, '$')) != 0)
    {
      if (!strncmp(p2, "$releasever", 11))
	{
	  if (!releaseevr)
	    {
	      void *rpmstate;
	      Queue q;
	
	      queue_init(&q);
	      rpmstate = rpm_state_create(pool, pool_get_rootdir(pool));
	      rpm_installedrpmdbids(rpmstate, "Providename", "system-release", &q);
	      if (q.count)
		{
		  void *handle;
		  char *p;
		  handle = rpm_byrpmdbid(rpmstate, q.elements[0]);
		  releaseevr = handle ? rpm_query(handle, SOLVABLE_EVR) : 0;
		  if (releaseevr && (p = strchr(releaseevr, '-')) != 0)
		    *p = 0;
		}
	      rpm_state_free(rpmstate);
	      queue_free(&q);
	      if (!releaseevr)
		{
		  fprintf(stderr, "no installed package provides 'system-release', cannot determine $releasever\n");
		  exit(1);
		}
	    }
	  *p2 = 0;
	  p = pool_tmpjoin(pool, line, releaseevr, p2 + 11);
	  p2 = p + (p2 - line);
	  line = p;
	  p = p2 + strlen(releaseevr);
	  continue;
	}
      if (!strncmp(p2, "$basearch", 9))
	{
	  if (!basearch)
	    {
	      struct utsname un;
	      if (uname(&un))
		{
		  perror("uname");
		  exit(1);
		}
	      basearch = strdup(un.machine);
	      if (basearch[0] == 'i' && basearch[1] && !strcmp(basearch + 2, "86"))
		basearch[1] = '3';
	    }
	  *p2 = 0;
	  p = pool_tmpjoin(pool, line, basearch, p2 + 9);
	  p2 = p + (p2 - line);
	  line = p;
	  p = p2 + strlen(basearch);
	  continue;
	}
      p = p2 + 1;
    }
  return line;
}

struct repoinfo *
read_repoinfos_yum(Pool *pool, int *nrepoinfosp)
{
  const char *reposdir = REPOINFO_PATH;
  char buf[4096];
  char buf2[4096], *kp, *vp, *kpe;
  DIR *dir;
  FILE *fp;
  struct dirent *ent;
  int l, rdlen;
  struct repoinfo *repoinfos = 0, *cinfo;
  int nrepoinfos = 0;

  rdlen = strlen(reposdir);
  dir = opendir(reposdir);
  if (!dir)
    {
      *nrepoinfosp = 0;
      return 0;
    }
  while ((ent = readdir(dir)) != 0)
    {
      if (ent->d_name[0] == '.')
	continue;
      l = strlen(ent->d_name);
      if (l < 6 || rdlen + 2 + l >= sizeof(buf) || strcmp(ent->d_name + l - 5, ".repo") != 0)
	continue;
      snprintf(buf, sizeof(buf), "%s/%s", reposdir, ent->d_name);
      if ((fp = fopen(buf, "r")) == 0)
	{
	  perror(buf);
	  continue;
	}
      cinfo = 0;
      while(fgets(buf2, sizeof(buf2), fp))
	{
	  l = strlen(buf2);
	  if (l == 0)
	    continue;
	  while (l && (buf2[l - 1] == '\n' || buf2[l - 1] == ' ' || buf2[l - 1] == '\t'))
	    buf2[--l] = 0;
	  kp = buf2;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp || *kp == '#')
	    continue;
	  if (strchr(kp, '$'))
	    kp = yum_substitute(pool, kp);
	  if (*kp == '[')
	    {
	      vp = strrchr(kp, ']');
	      if (!vp)
		continue;
	      *vp = 0;
	      repoinfos = solv_extend(repoinfos, nrepoinfos, 1, sizeof(*repoinfos), 15);
	      cinfo = repoinfos + nrepoinfos++;
	      memset(cinfo, 0, sizeof(*cinfo));
	      cinfo->alias = strdup(kp + 1);
	      cinfo->type = TYPE_RPMMD;
	      cinfo->autorefresh = 1;
	      cinfo->priority = 99;
#if !defined(FEDORA) && !defined(MAGEIA)
	      cinfo->repo_gpgcheck = 1;
#endif
	      cinfo->metadata_expire = METADATA_EXPIRE;
	      continue;
	    }
	  if (!cinfo)
	    continue;
          vp = strchr(kp, '=');
	  if (!vp)
	    continue;
	  for (kpe = vp - 1; kpe >= kp; kpe--)
	    if (*kpe != ' ' && *kpe != '\t')
	      break;
	  if (kpe == kp)
	    continue;
	  vp++;
	  while (*vp == ' ' || *vp == '\t')
	    vp++;
	  kpe[1] = 0;
	  if (!strcmp(kp, "name"))
	    cinfo->name = strdup(vp);
	  else if (!strcmp(kp, "enabled"))
	    cinfo->enabled = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "autorefresh"))
	    cinfo->autorefresh = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "gpgcheck"))
	    cinfo->pkgs_gpgcheck = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "repo_gpgcheck"))
	    cinfo->repo_gpgcheck = *vp == '0' ? 0 : 1;
	  else if (!strcmp(kp, "baseurl"))
	    cinfo->baseurl = strdup(vp);
	  else if (!strcmp(kp, "mirrorlist"))
	    {
	      if (strstr(vp, "metalink"))
	        cinfo->metalink = strdup(vp);
	      else
	        cinfo->mirrorlist = strdup(vp);
	    }
	  else if (!strcmp(kp, "path"))
	    {
	      if (vp && strcmp(vp, "/") != 0)
	        cinfo->path = strdup(vp);
	    }
	  else if (!strcmp(kp, "type"))
	    {
	      if (!strcmp(vp, "yast2"))
	        cinfo->type = TYPE_SUSETAGS;
	      else if (!strcmp(vp, "rpm-md"))
	        cinfo->type = TYPE_RPMMD;
	      else if (!strcmp(vp, "plaindir"))
	        cinfo->type = TYPE_PLAINDIR;
	      else if (!strcmp(vp, "mdk"))
	        cinfo->type = TYPE_MDK;
	      else
	        cinfo->type = TYPE_UNKNOWN;
	    }
	  else if (!strcmp(kp, "priority"))
	    cinfo->priority = atoi(vp);
	  else if (!strcmp(kp, "keeppackages"))
	    cinfo->keeppackages = *vp == '0' ? 0 : 1;
	}
      fclose(fp);
      cinfo = 0;
    }
  closedir(dir);
  *nrepoinfosp = nrepoinfos;
  return repoinfos;
}

#endif
