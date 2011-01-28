/*
 * Copyright (c) 2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "sat_xfopen.h"

static ssize_t cookie_gzread(void *cookie, char *buf, size_t nbytes)
{
  return gzread((gzFile *)cookie, buf, nbytes);
}

static int
cookie_gzclose(void *cookie)
{
  return gzclose((gzFile *)cookie);
}

static FILE *mygzfopen(gzFile* gzf)
{
#ifdef HAVE_FUNOPEN
  return funopen(
      gzf, (int (*)(void *, char *, int))cookie_gzread,
      (int (*)(void *, const char *, int))NULL, /* writefn */
      (fpos_t (*)(void *, fpos_t, int))NULL, /* seekfn */
      cookie_gzclose
      );
#elif defined(HAVE_FOPENCOOKIE)
  cookie_io_functions_t cio;
  memset(&cio, 0, sizeof(cio));
  cio.read = cookie_gzread;
  cio.close = cookie_gzclose;
  return  fopencookie(gzf, "r", cio);
#else
# error Need to implement custom I/O
#endif
}

FILE *
sat_xfopen(const char *fn)
{
  char *suf;
  gzFile *gzf;

  if (!fn)
    return 0;
  suf = strrchr(fn, '.');
  if (!suf || strcmp(suf, ".gz") != 0)
    return fopen(fn, "r");
  gzf = gzopen(fn, "r");
  if (!gzf)
    return 0;
  return mygzfopen(gzf);
}

FILE *
sat_xfopen_fd(const char *fn, int fd)
{
  char *suf;
  gzFile *gzf;

  if (!fn)
    return 0;
  suf = strrchr(fn, '.');
  if (!suf || strcmp(suf, ".gz") != 0)
    return fdopen(fd, "r");
  gzf = gzdopen(fd, "r");
  if (!gzf)
    return 0;
  return mygzfopen(gzf);
}

