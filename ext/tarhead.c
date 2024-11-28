/*
 * Copyright (c) 2012, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>

#include "util.h"
#include "tarhead.h"

static long long parsenum(unsigned char *p, int cnt)
{
  long long x = 0;
  if (!cnt)
    return -1;
  if (*p & 0x80)
    {
      /* binary format */
      x = *p & 0x40 ? (-1 << 8 | *p)  : (*p ^ 0x80);
      while (--cnt > 0)
	x = (x << 8) | *p++;
      return x;
    }
  while (cnt > 0 && (*p == ' ' || *p == '\t'))
    cnt--, p++;
  if (*p == '-')
    return -1;
  for (; cnt > 0 && *p >= '0' && *p < '8'; cnt--, p++)
    x = (x << 3) | (*p - '0');
  return x;
}

static int readblock(FILE *fp, unsigned char *blk)
{
  int r, l = 0;
  while (l < 512)
    {
      r = fread(blk + l, 1, 512 - l, fp);
      if (r <= 0)
	return -1;
      l += r;
    }
  return 0;
}

void tarhead_skip(struct tarhead *th)
{
  for (; th->length > 0; th->length -= 512)
    {
      if (readblock(th->fp, th->blk))
	{
	  th->eof = 1;
	  th->length = 0;
	  return;
	}
    }
  th->length = 0;
  th->off = th->end = 0;
}

void tarhead_init(struct tarhead *th, FILE *fp)
{
  memset(th, 0, sizeof(*th));
  th->fp = fp;
}

void tarhead_free(struct tarhead *th)
{
  solv_free(th->path);
}

int tarhead_next(struct tarhead *th)
{
  int l, type;
  long long length;

  th->path = solv_free(th->path);
  th->ispax = 0;
  th->type = 0;
  th->length = 0;
  th->off = 0;
  th->end = 0;
  if (th->eof)
    return 0;
  for (;;)
    {
      int r = readblock(th->fp, th->blk);
      if (r)
	{
	  if (feof(th->fp))
	    {
	      th->eof = 1;
	      return 0;
	    }
	  return -1;
	}
      if (th->blk[0] == 0)
	{
          th->eof = 1;
	  return 0;
	}
      length = parsenum(th->blk + 124, 12);
      if (length < 0)
	return -1;
      type = 0;
      switch (th->blk[156])
	{
	case 'S': case '0':
	  type = 1;	/* file */
	  break;
	case '1':
	  /* hard link, special length magic... */
	  if (!th->ispax)
	    length = 0;
	  break;
	case '5':
	  type = 2;	/* dir */
	  break;
	case '2': case '3': case '4': case '6':
	  length = 0;
	  break;
	case 'X': case 'x': case 'L':
	  {
	    char *data, *pp;
	    if (length < 1 || length >= 1024 * 1024)
	      return -1;
	    data = pp = solv_malloc(length + 512);
	    for (l = length; l > 0; l -= 512, pp += 512)
	      if (readblock(th->fp, (unsigned char *)pp))
	        {
		  solv_free(data);
		  return -1;
	        }
	    data[length] = 0;
	    type = 3;		/* extension */
	    if (th->blk[156] == 'L')
	      {
	        solv_free(th->path);
	        th->path = data;
	        length = 0;
		break;
	      }
	    pp = data;
	    while (length > 0)
	      {
		int ll = 0;
		for (l = 0; l < length && pp[l] >= '0' && pp[l] <= '9'; l++)
		  ll = ll * 10 + (pp[l] - '0');
		if (l == length || pp[l] != ' ' || ll < 1 || ll > length || pp[ll - 1] != '\n')
		  {
		    solv_free(data);
		    return -1;
		  }
		length -= ll;
		pp += l + 1;
		ll -= l + 1;
		pp[ll - 1] = 0;
		if (!strncmp(pp, "path=", 5))
		  {
		    solv_free(th->path);
		    th->path = solv_strdup(pp + 5);
		  }
		pp += ll;
	      }
	    solv_free(data);
	    th->ispax = 1;
	    length = 0;
	    break;
	  }
	default:
	  type = 3;	/* extension */
	  break;
	}
      if ((type == 1 || type == 2) && !th->path)
	{
	  char path[157];
	  memcpy(path, th->blk, 156);
	  path[156] = 0;
	  if (!memcmp(th->blk + 257, "ustar\0\060\060", 8) && !th->path && th->blk[345])
	    {
	      /* POSIX ustar with prefix */
	      char prefix[156];
	      memcpy(prefix, th->blk + 345, 155);
	      prefix[155] = 0;
	      l = strlen(prefix);
	      if (l && prefix[l - 1] == '/')
		prefix[l - 1] = 0;
	      th->path = solv_dupjoin(prefix, "/", path);
	    }
	  else
	    th->path = solv_dupjoin(path, 0, 0);
	}
      if (type == 1 || type == 2)
	{
	  l = strlen(th->path);
	  if (l && th->path[l - 1] == '/')
	    {
	      if (l > 1)
		th->path[l - 1] = 0;
	      type = 2;
	    }
	}
      if (type != 3)
	break;
      while (length > 0)
	{
	  r = readblock(th->fp, th->blk);
	  if (r)
	    return r;
	  length -= 512;
	}
    }
  th->type = type;
  th->length = length;
  return 1;
}

size_t tarhead_gets(struct tarhead *th, char **linep , size_t *allocsizep)
{
  char *line = *linep;
  size_t lsize = allocsizep ? *allocsizep : 0, size = 0;
  int i;
 
  if (th->eof)
    return 0;
  for (;;)
    {
      size_t fsize = lsize - size;
      if (fsize < 2)
	{
	  line = *linep = solv_realloc(line, lsize += 1024);
	  fsize = lsize - size;
	}
      for (i = th->off; i < th->end && fsize > 1;)
	{
	  fsize--;
	  if ((line[size++] = th->blk[i++]) == '\n')
	    {
	      th->off = i;
	      line[size] = 0;
	      return size;
	    }
	}
      /* end of block reached, read next block */
      th->off = i;
      if (th->off < th->end)
	continue;
      if (!th->path)
	{
	  /* fake entry */
	  th->off = 0;
	  th->end = fread(th->blk, 1, 512, th->fp);
	  if (th->end <= 0)
	    {
	      th->eof = 1;
	      if (th->end < 0)
		return 0;
	      break;
	    }
	  continue;
	}
      if (th->length <= 0)
	break;		/* reached end of entry */
      if (readblock(th->fp, th->blk))
	{
	  th->eof = 1;
	  return 0;
	}
      th->off = 0;
      th->end = th->length > 512 ? 512 : th->length;
      th->length -= th->end;
    }
  line[size] = 0;
  return size;
}

