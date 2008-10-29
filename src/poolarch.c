/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * poolarch.c
 *
 * create architecture policies
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "poolid.h"
#include "poolarch.h"
#include "util.h"

const char *archpolicies[] = {
  "x86_64",	"x86_64:i686:i586:i486:i386",
  "i686",	"i686:i586:i486:i386",
  "i586",	"i586:i486:i386",
  "i486",	"i486:i386",
  "i386",	"i386",
  "s390x",	"s390x:s390",
  "s390",	"s390",
  "ia64",	"ia64:i686:i586:i486:i386",
  "ppc64",	"ppc64:ppc",
  "ppc",	"ppc",
  "armv6l",	"armv6l:armv5tejl:armv5tel:armv5l:armv4tl:armv4l:armv3l",
  "armv5tejl",	"armv5tejl:armv5tel:armv5l:armv4tl:armv4l:armv3l",
  "armv5tel",	"armv5tel:armv5l:armv4tl:armv4l:armv3l",
  "armv5l",	"armv5l:armv4tl:armv4l:armv3l",
  "armv4tl",	"armv4tl:armv4l:armv3l",
  "armv4l",	"armv4l:armv3l",
  "armv3l",	"armv3l",
  "sh3",	"sh3",
  "sh4",	"sh4",
  "sh4a",	"sh4a:sh4",
  0
};

void
pool_setarch(Pool *pool, const char *arch)
{
  const char *a;
  char buf[256];
  unsigned int score = 0x10001;
  size_t l;
  char d;
  int i;
  Id *id2arch;
  Id id, lastarch;

  pool->id2arch = sat_free(pool->id2arch);
  if (!arch)
    {
      pool->lastarch = 0;
      return;
    }
  id = ARCH_NOARCH;
  lastarch = id + 255;
  id2arch = sat_calloc(lastarch + 1, sizeof(Id));
  id2arch[id] = 1;

  a = "";
  for (i = 0; archpolicies[i]; i += 2)
    if (!strcmp(archpolicies[i], arch))
      break;
  if (archpolicies[i])
    a = archpolicies[i + 1];
  d = 0;
  while (*a)
    {
      l = strcspn(a, ":=>");
      if (l && l < sizeof(buf) - 1)
	{
	  strncpy(buf, a, l);
	  buf[l] = 0;
	  id = str2id(pool, buf, 1);
	  if (id > lastarch)
	    {
	      id2arch = sat_realloc(id2arch, (id + 255 + 1) * sizeof(Id));
	      memset(id2arch + lastarch + 1, 0, (id + 255 - lastarch) * sizeof(Id));
	      lastarch = id + 255;
	    }
	  if (id2arch[id] == 0)
	    {
	      if (d == ':')
		score += 0x10000;
	      else if (d == '>')
		score += 0x00001;
	      id2arch[id] = score;
	    }
	}
      a += l;
      if ((d = *a++) == 0)
	break;
    }
  pool->id2arch = id2arch;
  pool->lastarch = lastarch;
}
