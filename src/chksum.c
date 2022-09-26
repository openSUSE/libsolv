/*
 * Copyright (c) 2008-2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "util.h"
#include "chksum.h"

#ifdef WITH_OPENSSL

#include <openssl/evp.h>

typedef EVP_MD_CTX* MD5_CTX;
typedef EVP_MD_CTX* SHA1_CTX;
typedef EVP_MD_CTX* SHA224_CTX;
typedef EVP_MD_CTX* SHA256_CTX;
typedef EVP_MD_CTX* SHA384_CTX;
typedef EVP_MD_CTX* SHA512_CTX;

#define solv_MD5_Init(ctx) { *ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(*ctx, EVP_md5(), NULL); }
#define solv_MD5_Update(ctx, data, len) EVP_DigestUpdate(*ctx, data, len)
#define solv_MD5_Final(md, ctx) EVP_DigestFinal_ex(*ctx, md, NULL)

#define solv_SHA1_Init(ctx) { *ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(*ctx, EVP_sha1(), NULL); }
#define solv_SHA1_Update(ctx, data, len) EVP_DigestUpdate(*ctx, data, len)
#define solv_SHA1_Final(ctx, md) EVP_DigestFinal_ex(*ctx, md, NULL)

#define solv_SHA224_Init(ctx) { *ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(*ctx, EVP_sha224(), NULL); }
#define solv_SHA224_Update(ctx, data, len) EVP_DigestUpdate(*ctx, data, len)
#define solv_SHA224_Final(md, ctx) EVP_DigestFinal_ex(*ctx, md, NULL)

#define solv_SHA256_Init(ctx) { *ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(*ctx, EVP_sha256(), NULL); }
#define solv_SHA256_Update(ctx, data, len) EVP_DigestUpdate(*ctx, data, len)
#define solv_SHA256_Final(md, ctx) EVP_DigestFinal_ex(*ctx, md, NULL)

#define solv_SHA384_Init(ctx) { *ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(*ctx, EVP_sha384(), NULL); }
#define solv_SHA384_Update(ctx, data, len) EVP_DigestUpdate(*ctx, data, len)
#define solv_SHA384_Final(md, ctx) EVP_DigestFinal_ex(*ctx, md, NULL)

#define solv_SHA512_Init(ctx) { *ctx = EVP_MD_CTX_new(); EVP_DigestInit_ex(*ctx, EVP_sha512(), NULL); }
#define solv_SHA512_Update(ctx, data, len) EVP_DigestUpdate(*ctx, data, len)
#define solv_SHA512_Final(md, ctx) EVP_DigestFinal_ex(*ctx, md, NULL)

#else

#include "md5.h"
#include "sha1.h"
#include "sha2.h"

#endif

#ifdef _WIN32
  #include "strfncs.h"
#endif

struct s_Chksum {
  Id type;
  int done;
  unsigned char result[64];
  union {
    MD5_CTX md5;
    SHA1_CTX sha1;
    SHA224_CTX sha224;
    SHA256_CTX sha256;
    SHA384_CTX sha384;
    SHA512_CTX sha512;
  } c;
};

#ifdef WITH_OPENSSL

void
openssl_ctx_copy(Chksum *chk_out, Chksum *chk_in)
{
  switch(chk_in->type)
    {
    case REPOKEY_TYPE_MD5:
      chk_out->c.md5 = EVP_MD_CTX_new();
      EVP_MD_CTX_copy_ex(chk_out->c.md5, chk_in->c.md5);
      return;
    case REPOKEY_TYPE_SHA1:
      chk_out->c.sha1 = EVP_MD_CTX_new();
      EVP_MD_CTX_copy_ex(chk_out->c.sha1, chk_in->c.sha1);
      return;
    case REPOKEY_TYPE_SHA224:
      chk_out->c.sha224 = EVP_MD_CTX_new();
      EVP_MD_CTX_copy_ex(chk_out->c.sha224, chk_in->c.sha224);
      return;
    case REPOKEY_TYPE_SHA256:
      chk_out->c.sha256 = EVP_MD_CTX_new();
      EVP_MD_CTX_copy_ex(chk_out->c.sha256, chk_in->c.sha256);
      return;
    case REPOKEY_TYPE_SHA384:
      chk_out->c.sha384 = EVP_MD_CTX_new();
      EVP_MD_CTX_copy_ex(chk_out->c.sha384, chk_in->c.sha384);
      return;
    case REPOKEY_TYPE_SHA512:
      chk_out->c.sha512 = EVP_MD_CTX_new();
      EVP_MD_CTX_copy_ex(chk_out->c.sha512, chk_in->c.sha512);
      return;
    default:
      return;
    }
}

void
openssl_ctx_free(Chksum *chk)
{
  switch(chk->type)
    {
    case REPOKEY_TYPE_MD5:
      EVP_MD_CTX_free(chk->c.md5);
      return;
    case REPOKEY_TYPE_SHA1:
      EVP_MD_CTX_free(chk->c.sha1);
      return;
    case REPOKEY_TYPE_SHA224:
      EVP_MD_CTX_free(chk->c.sha224);
      return;
    case REPOKEY_TYPE_SHA256:
      EVP_MD_CTX_free(chk->c.sha256);
      return;
    case REPOKEY_TYPE_SHA384:
      EVP_MD_CTX_free(chk->c.sha384);
      return;
    case REPOKEY_TYPE_SHA512:
      EVP_MD_CTX_free(chk->c.sha512);
      return;
    default:
      return;
    }
}

#endif

Chksum *
solv_chksum_create(Id type)
{
  Chksum *chk;
  chk = solv_calloc(1, sizeof(*chk));
  chk->type = type;
  switch(type)
    {
    case REPOKEY_TYPE_MD5:
      solv_MD5_Init(&chk->c.md5);
      return chk;
    case REPOKEY_TYPE_SHA1:
      solv_SHA1_Init(&chk->c.sha1);
      return chk;
    case REPOKEY_TYPE_SHA224:
      solv_SHA224_Init(&chk->c.sha224);
      return chk;
    case REPOKEY_TYPE_SHA256:
      solv_SHA256_Init(&chk->c.sha256);
      return chk;
    case REPOKEY_TYPE_SHA384:
      solv_SHA384_Init(&chk->c.sha384);
      return chk;
    case REPOKEY_TYPE_SHA512:
      solv_SHA512_Init(&chk->c.sha512);
      return chk;
    default:
      break;
    }
  free(chk);
  return 0;
}

Chksum *
solv_chksum_create_clone(Chksum *chk)
{
  Chksum *chk_clone = solv_memdup(chk, sizeof(*chk));
#ifdef WITH_OPENSSL
  openssl_ctx_copy(chk_clone, chk);
#endif
  return chk_clone;
}

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
  chk->done = 1;
  memcpy(chk->result, buf, l);
  return chk;
}

void
solv_chksum_add(Chksum *chk, const void *data, int len)
{
  if (chk->done)
    return;
  switch(chk->type)
    {
    case REPOKEY_TYPE_MD5:
      solv_MD5_Update(&chk->c.md5, (void *)data, len);
      return;
    case REPOKEY_TYPE_SHA1:
      solv_SHA1_Update(&chk->c.sha1, data, len);
      return;
    case REPOKEY_TYPE_SHA224:
      solv_SHA224_Update(&chk->c.sha224, data, len);
      return;
    case REPOKEY_TYPE_SHA256:
      solv_SHA256_Update(&chk->c.sha256, data, len);
      return;
    case REPOKEY_TYPE_SHA384:
      solv_SHA384_Update(&chk->c.sha384, data, len);
      return;
    case REPOKEY_TYPE_SHA512:
      solv_SHA512_Update(&chk->c.sha512, data, len);
      return;
    default:
      return;
    }
}

const unsigned char *
solv_chksum_get(Chksum *chk, int *lenp)
{
  if (chk->done)
    {
      if (lenp)
        *lenp = solv_chksum_len(chk->type);
      return chk->result;
    }
  switch(chk->type)
    {
    case REPOKEY_TYPE_MD5:
      solv_MD5_Final(chk->result, &chk->c.md5);
      chk->done = 1;
      if (lenp)
	*lenp = 16;
      return chk->result;
    case REPOKEY_TYPE_SHA1:
      solv_SHA1_Final(&chk->c.sha1, chk->result);
      chk->done = 1;
      if (lenp)
	*lenp = 20;
      return chk->result;
    case REPOKEY_TYPE_SHA224:
      solv_SHA224_Final(chk->result, &chk->c.sha224);
      chk->done = 1;
      if (lenp)
	*lenp = 28;
      return chk->result;
    case REPOKEY_TYPE_SHA256:
      solv_SHA256_Final(chk->result, &chk->c.sha256);
      chk->done = 1;
      if (lenp)
	*lenp = 32;
      return chk->result;
    case REPOKEY_TYPE_SHA384:
      solv_SHA384_Final(chk->result, &chk->c.sha384);
      chk->done = 1;
      if (lenp)
	*lenp = 48;
      return chk->result;
    case REPOKEY_TYPE_SHA512:
      solv_SHA512_Final(chk->result, &chk->c.sha512);
      chk->done = 1;
      if (lenp)
	*lenp = 64;
      return chk->result;
    default:
      if (lenp)
	*lenp = 0;
      return 0;
    }
}

Id
solv_chksum_get_type(Chksum *chk)
{
  return chk->type;
}

int
solv_chksum_isfinished(Chksum *chk)
{
  return chk->done != 0;
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
  if (cp)
    {
      const unsigned char *res;
      int l;
      res = solv_chksum_get(chk, &l);
      if (l && res)
        memcpy(cp, res, l);
    }
#ifdef WITH_OPENSSL
  openssl_ctx_free(chk);
#endif
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
  return memcmp(res1, res2, len) == 0 ? 1 : 0;
}
