
/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * util.h
 *
 */

#ifndef SATSOLVER_TOOLS_UTIL_H
#define SATSOLVER_TOOLS_UTIL_H

struct parsedata_common {
  char *tmp;
  int tmpl;
  Pool *pool;
  Repo *repo;
};

static Id
makeevr(Pool *pool, char *s)
{
  if (!strncmp(s, "0:", 2) && s[2])
    s += 2;
  return str2id(pool, s, 1);
}

/**
 * split a string
 */
static int
split(char *l, char **sp, int m)
{
  int i;
  for (i = 0; i < m;)
    {
      while (*l == ' ')
        l++;
      if (!*l)
        break;
      sp[i++] = l;
      while (*l && *l != ' ')
        l++;
      if (!*l)
        break;
      *l++ = 0;
    }
  return i;
}

static char *
join(struct parsedata_common *pd, const char *s1, const char *s2, const char *s3)
{
  int l = 1;
  char *p;

  if (s1)
    l += strlen(s1);
  if (s2)
    l += strlen(s2);
  if (s3)
    l += strlen(s3);
  if (l > pd->tmpl)
    {
      pd->tmpl = l + 256;
      if (!pd->tmp)
        pd->tmp = malloc(pd->tmpl);
      else
        pd->tmp = realloc(pd->tmp, pd->tmpl);
    }
  p = pd->tmp;
  if (s1)
    {
      strcpy(p, s1);
      p += strlen(s1);
    }
  if (s2)
    {
      strcpy(p, s2);
      p += strlen(s2);
    }
  if (s3)
    {
      strcpy(p, s3);
      p += strlen(s3);
    }
  return pd->tmp;
}

#endif

