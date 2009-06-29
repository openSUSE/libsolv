#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pool.h"
#include "util.h"
#include "chksum.h"

#include "md5.h"
#include "sha1.h"
#include "sha2.h"

struct ctxhandle {
  Id type;
  int done;
  unsigned char result[64];
  union {
    MD5_CTX md5;
    SHA1_CTX sha1;
    SHA256_CTX sha256;
  } c;
};

void *
sat_chksum_create(Id type)
{
  struct ctxhandle *h;
  h = sat_calloc(1, sizeof(*h));
  h->type = type;
  switch(type)
    {
    case REPOKEY_TYPE_MD5:
      sat_MD5_Init(&h->c.md5);
      return h;
    case REPOKEY_TYPE_SHA1:
      sat_SHA1_Init(&h->c.sha1);
      return h;
    case REPOKEY_TYPE_SHA256:
      sat_SHA256_Init(&h->c.sha256);
      return h;
    }
  free(h);
  return 0;
}

void
sat_chksum_add(void *handle, const void *data, int len)
{
  struct ctxhandle *h = handle;
  switch(h->type)
    {
    case REPOKEY_TYPE_MD5:
      sat_MD5_Update(&h->c.md5, (void *)data, len);
      return;
    case REPOKEY_TYPE_SHA1:
      sat_SHA1_Update(&h->c.sha1, data, len);
      return;
    case REPOKEY_TYPE_SHA256:
      sat_SHA256_Update(&h->c.sha256, data, len);
      return;
    default:
      return;
    }
}

unsigned char *
sat_chksum_get(void *handle, int *lenp)
{
  struct ctxhandle *h = handle;
  if (h->done)
    return h->result;
  switch(h->type)
    {
    case REPOKEY_TYPE_MD5:
      sat_MD5_Final(h->result, &h->c.md5);
      h->done = 1;
      if (*lenp)
	*lenp = 16;
      return h->result;
    case REPOKEY_TYPE_SHA1:
      sat_SHA1_Final(&h->c.sha1, h->result);
      h->done = 1;
      if (*lenp)
	*lenp = 20;
      return h->result;
    case REPOKEY_TYPE_SHA256:
      sat_SHA256_Final(h->result, &h->c.sha256);
      h->done = 1;
      if (*lenp)
	*lenp = 32;
      return h->result;
    default:
      if (*lenp)
	*lenp = 0;
      return 0;
    }
}

void *
sat_chksum_free(void *handle, unsigned char *cp)
{
  if (cp)
    {
      unsigned char *res;
      int l;
      res = sat_chksum_get(handle, &l);
      memcpy(cp, res, l);
    }
  sat_free(handle);
  return 0;
}

