#include "pool.h"

void *sat_chksum_create(Id type);
void *sat_chksum_create_from_bin(Id type, const unsigned char *buf);
void sat_chksum_add(void *handle, const void *data, int len);
Id sat_chksum_get_type(void *handle);
const unsigned char *sat_chksum_get(void *handle, int *lenp);
void *sat_chksum_free(void *handle, unsigned char *cp);

static inline int sat_chksum_len(Id type)
{
  switch (type)
    {
    case REPOKEY_TYPE_MD5:
      return 16;
    case REPOKEY_TYPE_SHA1:
      return 20;
    case REPOKEY_TYPE_SHA256:
      return 32;
    default:
      return 0;
    }
}
