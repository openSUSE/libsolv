#if defined(MANDRIVA) || defined(MAGEIA)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>

#include "pool.h"
#include "repo.h"

#include "repoinfo.h"
#include "repoinfo_config_urpmi.h"


#define URPMI_CFG "/etc/urpmi/urpmi.cfg"


struct repoinfo *
read_repoinfos_urpmi(Pool *pool, int *nrepoinfosp)
{
  char buf[4096], *bp, *arg;
  FILE *fp;
  int l, insect = 0;
  struct repoinfo *cinfo = 0;
  struct repoinfo *repoinfos = 0;
  int nrepoinfos = 0;

  if ((fp = fopen(URPMI_CFG, "r")) == 0)
    {
      *nrepoinfosp = 0;
      return 0;
    }
  while (fgets(buf, sizeof(buf), fp))
    {
      l = strlen(buf);
      while (l && (buf[l - 1] == '\n' || buf[l - 1] == ' ' || buf[l - 1] == '\t'))
	buf[--l] = 0;
      bp = buf;
      while (l && (*bp == ' ' || *bp == '\t'))
	{
	  l--;
	  bp++;
	}
      if (!l || *bp == '#')
	continue;
      if (!insect && bp[l - 1] == '{')
	{
	  insect++;
	  bp[--l] = 0;
	  if (l > 0)
	    {
	      while (l && (bp[l - 1] == ' ' || bp[l - 1] == '\t'))
		bp[--l] = 0;
	    }
	  if (l)
	    {
	      char *bbp = bp, *bbp2 = bp;
	      /* unescape */
	      while (*bbp)
		{
		  if (*bbp == '\\' && bbp[1])
		    bbp++;
		  *bbp2++ = *bbp++;
		}
	      *bbp2 = 0;
	      repoinfos = solv_extend(repoinfos, nrepoinfos, 1, sizeof(*repoinfos), 15);
	      cinfo = repoinfos + nrepoinfos++;
	      memset(cinfo, 0, sizeof(*cinfo));
	      cinfo->alias = strdup(bp);
	      cinfo->type = TYPE_MDK;
	      cinfo->autorefresh = 1;
	      cinfo->priority = 99;
	      cinfo->enabled = 1;
	      cinfo->metadata_expire = METADATA_EXPIRE;
	    }
	  continue;
	}
      if (insect && *bp == '}')
	{
	  insect--;
	  cinfo = 0;
	  continue;
	}
      if (!insect || !cinfo)
	continue;
      if ((arg = strchr(bp, ':')) != 0)
	{
	  *arg++ = 0;
	  while (*arg == ' ' || *arg == '\t')
	    arg++;
	  if (!*arg)
	    arg = 0;
	}
      if (strcmp(bp, "ignore") == 0)
	cinfo->enabled = 0;
      if (strcmp(bp, "mirrorlist") == 0)
	cinfo->mirrorlist = solv_strdup(arg);
      if (strcmp(bp, "with-dir") == 0)
	cinfo->path = solv_strdup(arg);
    }
  fclose(fp);
  *nrepoinfosp = nrepoinfos;
  return repoinfos;
}

#endif
