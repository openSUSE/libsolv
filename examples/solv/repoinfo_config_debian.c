#ifdef DEBIAN

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>

#include "pool.h"
#include "repo.h"

#include "repoinfo.h"
#include "repoinfo_config_debian.h"



struct repoinfo *
read_repoinfos_debian(Pool *pool, int *nrepoinfosp)
{
  FILE *fp;
  char buf[4096];
  char buf2[4096];
  int l;
  char *kp, *url, *distro;
  struct repoinfo *repoinfos = 0, *cinfo;
  int nrepoinfos = 0;
  DIR *dir = 0;
  struct dirent *ent;

  fp = fopen("/etc/apt/sources.list", "r");
  while (1)
    {
      if (!fp)
	{
	  if (!dir)
	    {
	      dir = opendir("/etc/apt/sources.list.d");
	      if (!dir)
		break;
	    }
	  if ((ent = readdir(dir)) == 0)
	    {
	      closedir(dir);
	      break;
	    }
	  if (ent->d_name[0] == '.')
	    continue;
	  l = strlen(ent->d_name);
	  if (l < 5 || strcmp(ent->d_name + l - 5, ".list") != 0)
	    continue;
	  snprintf(buf, sizeof(buf), "%s/%s", "/etc/apt/sources.list.d", ent->d_name);
	  if (!(fp = fopen(buf, "r")))
	    continue;
	}
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
	  if (strncmp(kp, "deb", 3) != 0)
	    continue;
	  kp += 3;
	  if (*kp != ' ' && *kp != '\t')
	    continue;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp)
	    continue;
	  url = kp;
	  while (*kp && *kp != ' ' && *kp != '\t')
	    kp++;
	  if (*kp)
	    *kp++ = 0;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp)
	    continue;
	  distro = kp;
	  while (*kp && *kp != ' ' && *kp != '\t')
	    kp++;
	  if (*kp)
	    *kp++ = 0;
	  while (*kp == ' ' || *kp == '\t')
	    kp++;
	  if (!*kp)
	    continue;
	  repoinfos = solv_extend(repoinfos, nrepoinfos, 1, sizeof(*repoinfos), 15);
	  cinfo = repoinfos + nrepoinfos++;
	  memset(cinfo, 0, sizeof(*cinfo));
	  cinfo->baseurl = strdup(url);
	  cinfo->alias = solv_dupjoin(url, "/", distro);
	  cinfo->name = strdup(distro);
	  cinfo->type = TYPE_DEBIAN;
	  cinfo->enabled = 1;
	  cinfo->autorefresh = 1;
	  cinfo->repo_gpgcheck = 1;
	  cinfo->metadata_expire = METADATA_EXPIRE;
	  while (*kp)
	    {
	      char *compo;
	      while (*kp == ' ' || *kp == '\t')
		kp++;
	      if (!*kp)
		break;
	      compo = kp;
	      while (*kp && *kp != ' ' && *kp != '\t')
		kp++;
	      if (*kp)
		*kp++ = 0;
	      cinfo->components = solv_extend(cinfo->components, cinfo->ncomponents, 1, sizeof(*cinfo->components), 15);
	      cinfo->components[cinfo->ncomponents++] = strdup(compo);
	    }
	}
      fclose(fp);
      fp = 0;
    }
  *nrepoinfosp = nrepoinfos;
  return repoinfos;
}

#endif
