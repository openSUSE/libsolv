/*
 * Copyright (c) 2008-2026, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef _WIN32
#include <strings.h>
#endif

#include "pool.h"
#include "util.h"
#include "chksum.h"

#include "md5.h"
#include "sha1.h"
#include "sha2.h"

#ifdef _WIN32
  #include "strfncs.h"
#endif

/* keep in sync with chksum_impl.c */
struct s_Chksum {
  Id type;
  void * (*impl)(struct s_Chksum *, int);
  unsigned char result[SOLV_CHKSUM_MAXLEN];
};

int
solv_chksum_len(Id type)
{
  switch (type)
    {
    case REPOKEY_TYPE_MD5:
      return 16;
    case REPOKEY_TYPE_SHA1:
      return 20;
    case REPOKEY_TYPE_SHA224:
      return 28;
    case REPOKEY_TYPE_SHA256:
      return 32;
    case REPOKEY_TYPE_SHA384:
      return 48;
    case REPOKEY_TYPE_SHA512:
      return 64;
    default:
      return 0;
    }
}

Chksum *
solv_chksum_create_from_bin(Id type, const unsigned char *buf)
{
  Chksum *chk;
  int l = solv_chksum_len(type);
  if (buf == 0 || l == 0)
    return 0;
  chk = solv_calloc(1, sizeof(*chk));
  chk->type = type;
  memcpy(chk->result, buf, l);
  return chk;
}

Chksum *
solv_chksum_create_clone(Chksum *chk)
{
  if (chk->impl)
    return chk->impl(chk, SOLV_CHKSUMP_IMPL_CLONE);
  return solv_memdup(chk, sizeof(*chk));
}

static inline int
solv_chksum_finalize(Chksum *chk)
{
  unsigned char *end = chk->impl(chk, SOLV_CHKSUMP_IMPL_FINALIZE);
  return end ? end - chk->result : 0;
}

const unsigned char *
solv_chksum_get(Chksum *chk, int *lenp)
{
  int len = chk->impl ? solv_chksum_finalize(chk) : solv_chksum_len(chk->type);
  if (lenp)
    *lenp = len;
  return len ? chk->result : 0;
}

Id
solv_chksum_get_type(Chksum *chk)
{
  return chk->type;
}

int
solv_chksum_isfinished(Chksum *chk)
{
  return !chk->impl;
}

const char *
solv_chksum_type2str(Id type)
{
  switch(type)
    {
    case REPOKEY_TYPE_MD5:
      return "md5";
    case REPOKEY_TYPE_SHA1:
      return "sha1";
    case REPOKEY_TYPE_SHA224:
      return "sha224";
    case REPOKEY_TYPE_SHA256:
      return "sha256";
    case REPOKEY_TYPE_SHA384:
      return "sha384";
    case REPOKEY_TYPE_SHA512:
      return "sha512";
    default:
      return 0;
    }
}

Id
solv_chksum_str2type(const char *str)
{
  if (!strcasecmp(str, "md5"))
    return REPOKEY_TYPE_MD5;
  if (!strcasecmp(str, "sha") || !strcasecmp(str, "sha1"))
    return REPOKEY_TYPE_SHA1;
  if (!strcasecmp(str, "sha224"))
    return REPOKEY_TYPE_SHA224;
  if (!strcasecmp(str, "sha256"))
    return REPOKEY_TYPE_SHA256;
  if (!strcasecmp(str, "sha384"))
    return REPOKEY_TYPE_SHA384;
  if (!strcasecmp(str, "sha512"))
    return REPOKEY_TYPE_SHA512;
  return 0;
}

void *
solv_chksum_free(Chksum *chk, unsigned char *cp)
{
  if (!chk)
    return 0;
  if (cp)
    {
      int len = chk->impl ? solv_chksum_finalize(chk) : solv_chksum_len(chk->type);
      if (len)
        memcpy(cp, chk->result, len);
    }
  else if (chk->impl)
    chk->impl(chk, SOLV_CHKSUMP_IMPL_FREE);	/* free resources */
  solv_free(chk);
  return 0;
}

int
solv_chksum_cmp(Chksum *chk, Chksum *chk2)
{
  int len;
  const unsigned char *res1, *res2;
  if (chk == chk2)
    return 1;
  if (!chk || !chk2 || chk->type != chk2->type)
    return 0;
  res1 = solv_chksum_get(chk, &len);
  res2 = solv_chksum_get(chk2, 0);
  return res1 && res2 && memcmp(res1, res2, len) == 0 ? 1 : 0;
}
