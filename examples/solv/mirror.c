#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "pool.h"
#include "util.h"
#include "fastestmirror.h"

#include "mirror.h"

char *
findmetalinkurl(FILE *fp, unsigned char *chksump, Id *chksumtypep)
{
  char buf[4096], *bp, *ep;
  char **urls = 0;
  int nurls = 0;
  int i;

  if (chksumtypep)
    *chksumtypep = 0;
  while((bp = fgets(buf, sizeof(buf), fp)) != 0)
    {
      while (*bp == ' ' || *bp == '\t')
	bp++;
      if (chksumtypep && !*chksumtypep && !strncmp(bp, "<hash type=\"sha256\">", 20))
	{
	  bp += 20;
	  if (solv_hex2bin((const char **)&bp, chksump, 32) == 32)
	    *chksumtypep = REPOKEY_TYPE_SHA256;
	  continue;
	}
      if (strncmp(bp, "<url", 4))
	continue;
      bp = strchr(bp, '>');
      if (!bp)
	continue;
      bp++;
      ep = strstr(bp, "repodata/repomd.xml</url>");
      if (!ep)
	continue;
      *ep = 0;
      if (strncmp(bp, "http", 4))
	continue;
      urls = solv_extend(urls, nurls, 1, sizeof(*urls), 15);
      urls[nurls++] = strdup(bp);
    }
  if (nurls)
    {
      if (nurls > 1)
        findfastest(urls, nurls > 5 ? 5 : nurls);
      bp = urls[0];
      urls[0] = 0;
      for (i = 0; i < nurls; i++)
        solv_free(urls[i]);
      solv_free(urls);
      ep = strchr(bp, '/');
      if ((ep = strchr(ep + 2, '/')) != 0)
	{
	  *ep = 0;
	  printf("[using mirror %s]\n", bp);
	  *ep = '/';
	}
      return bp;
    }
  return 0;
}

char *
findmirrorlisturl(FILE *fp)
{
  char buf[4096], *bp, *ep;
  int i, l;
  char **urls = 0;
  int nurls = 0;

  while((bp = fgets(buf, sizeof(buf), fp)) != 0)
    {
      while (*bp == ' ' || *bp == '\t')
	bp++;
      if (!*bp || *bp == '#')
	continue;
      l = strlen(bp);
      while (l > 0 && (bp[l - 1] == ' ' || bp[l - 1] == '\t' || bp[l - 1] == '\n'))
	bp[--l] = 0;
      if ((ep = strstr(bp, "url=")) != 0)
	bp = ep + 4;
      urls = solv_extend(urls, nurls, 1, sizeof(*urls), 15);
      urls[nurls++] = strdup(bp);
    }
  if (nurls)
    {
      if (nurls > 1)
        findfastest(urls, nurls > 5 ? 5 : nurls);
      bp = urls[0];
      urls[0] = 0;
      for (i = 0; i < nurls; i++)
        solv_free(urls[i]);
      solv_free(urls);
      ep = strchr(bp, '/');
      if ((ep = strchr(ep + 2, '/')) != 0)
	{
	  *ep = 0;
	  printf("[using mirror %s]\n", bp);
	  *ep = '/';
	}
      return bp;
    }
  return 0;
}
