/*
 * Copyright (c) 2026, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#include <stddef.h>
#include <stdint.h>
#include <openssl/evp.h>

#include "pool.h"
#include "util.h"
#include "chksum.h"

/* keep in sync with chksum.c */
struct s_Chksum {
  Id type;
  void *(*impl)(struct s_Chksum *, int op);
  unsigned char result[SOLV_CHKSUM_MAXLEN];
  EVP_MD_CTX *context;
  int update_failed;
};

static void *
solv_chksum_impl(Chksum *chk, int op)
{
  if (op == SOLV_CHKSUMP_IMPL_CLONE) {
    Chksum *clone = solv_memdup(chk, sizeof(*chk));
    if (!(clone->context = EVP_MD_CTX_dup(chk->context))) {
      solv_free(clone);
      return 0;
    }
    return clone;
  }
  else if (op == SOLV_CHKSUMP_IMPL_FINALIZE)
  {
    unsigned int chk_size;
    chk->update_failed |= !EVP_DigestFinal_ex(chk->context, chk->result, &chk_size);
    EVP_MD_CTX_free(chk->context);
    chk->impl = 0;
    if (chk->update_failed)
      return 0;
    else
      return chk->result + chk_size;
  }
  else if (op == SOLV_CHKSUMP_IMPL_FREE)
  {
    EVP_MD_CTX_free(chk->context);
    chk->impl = 0;
    return 0;
  }
  else {
    return 0;
  }
}

Chksum *
solv_chksum_create(Id type)
{
  Chksum *chk;
  const EVP_MD *evp_type;

  chk = solv_calloc(1, sizeof(*chk));
  chk->type = type;
  chk->impl = solv_chksum_impl;

  if (!(chk->context = EVP_MD_CTX_new())) {
    solv_free(chk);
    return 0;
  }

  switch(type)
    {
    case REPOKEY_TYPE_MD5:
      evp_type = EVP_md5();
      break;
    case REPOKEY_TYPE_SHA1:
      evp_type = EVP_sha1();
      break;
    case REPOKEY_TYPE_SHA224:
      evp_type = EVP_sha224();
      break;
    case REPOKEY_TYPE_SHA256:
      evp_type = EVP_sha256();
      break;
    case REPOKEY_TYPE_SHA384:
      evp_type = EVP_sha384();
      break;
    case REPOKEY_TYPE_SHA512:
      evp_type = EVP_sha512();
      break;
    default:
      evp_type = 0;
      break;
    }
  if (evp_type && EVP_DigestInit_ex2(chk->context, evp_type, NULL))
    return chk;

  EVP_MD_CTX_free(chk->context);
  solv_free(chk);
  return 0;
}

void
solv_chksum_add(Chksum *chk, const void *data, int len)
{
  if (!chk)
    return;
  if (!chk->impl)
    return;
  if (len < 0 || len > SIZE_MAX) {
    chk->update_failed = 1;
    return;
  }
  if (!EVP_DigestUpdate(chk->context, data, (size_t)len))
    chk->update_failed = 1;
}

