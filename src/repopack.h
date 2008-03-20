/*
 * Copyright (c) 2007, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/* pack/unpack functions for key data */

#ifndef SATSOLVER_REPOPACK_H
#define SATSOLVER_REPOPACK_H

static inline unsigned char *
data_read_id(unsigned char *dp, Id *idp)
{
  Id x = 0;
  unsigned char c;
  for (;;)
    {
      c = *dp++;
      if (!(c & 0x80))
        {
          *idp = (x << 7) ^ c;
          return dp;
        }
      x = (x << 7) ^ c ^ 128;
    }
}

static inline unsigned char *
data_read_ideof(unsigned char *dp, Id *idp, int *eof)
{
  Id x = 0;
  unsigned char c;
  for (;;)
    {
      c = *dp++;
      if (!(c & 0x80))
        {
          if (c & 0x40)
            {
              c ^= 0x40;
              *eof = 0;
            }
          else
            *eof = 1;
          *idp = (x << 6) ^ c;
          return dp;
        }
      x = (x << 7) ^ c ^ 128;
    }
}

static inline unsigned char *
data_read_u32(unsigned char *dp, unsigned int *nump)
{
  *nump = (dp[0] << 24) | (dp[1] << 16) | (dp[2] << 8) | dp[3];
  return dp + 4;
}

static inline unsigned char *
data_fetch(unsigned char *dp, KeyValue *kv, Repokey *key)
{
  kv->eof = 1;
  if (!dp)
    return 0;
  switch (key->type)
    {
    case REPOKEY_TYPE_VOID:
      return dp;
    case REPOKEY_TYPE_CONSTANT:
      kv->num = key->size;
      return dp;
    case REPOKEY_TYPE_CONSTANTID:
      kv->id = key->size;
      return dp;
    case REPOKEY_TYPE_STR:
      kv->str = (const char *)dp;
      return dp + strlen(kv->str) + 1;
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_DIR:
      return data_read_id(dp, &kv->id);
    case REPOKEY_TYPE_NUM:
      return data_read_id(dp, &kv->num);
    case REPOKEY_TYPE_U32:
      return data_read_u32(dp, (unsigned int *)&kv->num);
    case REPOKEY_TYPE_MD5:
      kv->str = (const char *)dp;
      return dp + SIZEOF_MD5;
    case REPOKEY_TYPE_SHA1:
      kv->str = (const char *)dp;
      return dp + SIZEOF_SHA1;
    case REPOKEY_TYPE_SHA256:
      kv->str = (const char *)dp;
      return dp + SIZEOF_SHA256;
    case REPOKEY_TYPE_IDARRAY:
      return data_read_ideof(dp, &kv->id, &kv->eof);
    case REPOKEY_TYPE_DIRSTRARRAY:
      dp = data_read_ideof(dp, &kv->id, &kv->eof);
      kv->str = (const char *)dp;
      return dp + strlen(kv->str) + 1;
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      dp = data_read_id(dp, &kv->id);
      dp = data_read_id(dp, &kv->num);
      return data_read_ideof(dp, &kv->num2, &kv->eof);
    default:
      return 0;
    }
}

static inline unsigned char *
data_skip(unsigned char *dp, int type)
{
  unsigned char x;
  switch (type)
    {
    case REPOKEY_TYPE_VOID:
    case REPOKEY_TYPE_CONSTANT:
    case REPOKEY_TYPE_CONSTANTID:
      return dp;
    case REPOKEY_TYPE_ID:
    case REPOKEY_TYPE_NUM:
    case REPOKEY_TYPE_DIR:
      while ((*dp & 0x80) != 0)
        dp++;
      return dp + 1;
    case REPOKEY_TYPE_U32:
      return dp + 4;
    case REPOKEY_TYPE_MD5:
      return dp + SIZEOF_MD5;
    case REPOKEY_TYPE_SHA1:
      return dp + SIZEOF_SHA1;
    case REPOKEY_TYPE_SHA256:
      return dp + SIZEOF_SHA256;
    case REPOKEY_TYPE_IDARRAY:
    case REPOKEY_TYPE_REL_IDARRAY:
      while ((*dp & 0xc0) != 0)
        dp++;
      return dp + 1;
    case REPOKEY_TYPE_STR:
      while ((*dp) != 0)
        dp++;
      return dp + 1;
    case REPOKEY_TYPE_DIRSTRARRAY:
      for (;;)
        {
          while ((*dp & 0x80) != 0)
            dp++;
          x = *dp++;
          while ((*dp) != 0)
            dp++;
          dp++;
          if (!(x & 0x40))
            return dp;
        }
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      for (;;)
        {
          while ((*dp & 0x80) != 0)
            dp++;
          dp++;
          while ((*dp & 0x80) != 0)
            dp++;
          dp++;
          while ((*dp & 0x80) != 0)
            dp++;
          if (!(*dp & 0x40))
            return dp + 1;
          dp++;
        }
    default:
      return 0;
    }
}

static inline unsigned char *
data_skip_verify(unsigned char *dp, int type, int maxid, int maxdir)
{
  Id id;
  int eof;

  switch (type)
    {
    case REPOKEY_TYPE_VOID:
    case REPOKEY_TYPE_CONSTANT:
    case REPOKEY_TYPE_CONSTANTID:
      return dp;
    case REPOKEY_TYPE_NUM:
      while ((*dp & 0x80) != 0)
        dp++;
      return dp + 1;
    case REPOKEY_TYPE_U32:
      return dp + 4;
    case REPOKEY_TYPE_MD5:
      return dp + SIZEOF_MD5;
    case REPOKEY_TYPE_SHA1:
      return dp + SIZEOF_SHA1;
    case REPOKEY_TYPE_SHA256:
      return dp + SIZEOF_SHA256;
    case REPOKEY_TYPE_ID:
      dp = data_read_id(dp, &id);
      if (id >= maxid)
	return 0;
      return dp;
    case REPOKEY_TYPE_DIR:
      dp = data_read_id(dp, &id);
      if (id >= maxdir)
	return 0;
      return dp;
    case REPOKEY_TYPE_IDARRAY:
      for (;;)
	{
	  dp = data_read_ideof(dp, &id, &eof);
	  if (id >= maxid)
	    return 0;
	  if (eof)
	    return dp;
	}
    case REPOKEY_TYPE_STR:
      while ((*dp) != 0)
        dp++;
      return dp + 1;
    case REPOKEY_TYPE_DIRSTRARRAY:
      for (;;)
        {
	  dp = data_read_ideof(dp, &id, &eof);
	  if (id >= maxdir)
	    return 0;
          while ((*dp) != 0)
            dp++;
          dp++;
          if (eof)
            return dp;
        }
    case REPOKEY_TYPE_DIRNUMNUMARRAY:
      for (;;)
        {
	  dp = data_read_id(dp, &id);
	  if (id >= maxdir)
	    return 0;
          while ((*dp & 0x80) != 0)
            dp++;
          dp++;
          while ((*dp & 0x80) != 0)
            dp++;
          if (!(*dp & 0x40))
            return dp + 1;
          dp++;
        }
    default:
      return 0;
    }
}

#endif	/* SATSOLVER_REPOPACK */
