/*
 * Copyright (c) 2007-2012, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

#ifndef LIBSOLV_CHKSUM_H
#define LIBSOLV_CHKSUM_H

#include "pool.h"

#ifdef __cplusplus
extern "C" {
#endif

struct s_Chksum;
typedef struct s_Chksum Chksum;

Chksum *solv_chksum_create_from_bin(Id type, const unsigned char *buf);
Chksum *solv_chksum_create_clone(Chksum *chk);
Id solv_chksum_get_type(Chksum *chk);
int solv_chksum_isfinished(Chksum *chk);
const unsigned char *solv_chksum_get(Chksum *chk, int *lenp);
void *solv_chksum_free(Chksum *chk, unsigned char *cp);
const char *solv_chksum_type2str(Id type);
Id solv_chksum_str2type(const char *str);
int solv_chksum_len(Id type);
int solv_chksum_cmp(Chksum *chk, Chksum *chk2);

/* implementation */
Chksum *solv_chksum_create(Id type);
void solv_chksum_add(Chksum *chk, const void *data, int len);

#define SOLV_CHKSUM_MAXLEN 64

#define SOLV_CHKSUMP_IMPL_FREE  0
#define SOLV_CHKSUMP_IMPL_FINALIZE 1
#define SOLV_CHKSUMP_IMPL_CLONE 2

#ifdef LIBSOLV_INTERNAL

#define case_CHKSUM_TYPES \
    case REPOKEY_TYPE_MD5: \
    case REPOKEY_TYPE_SHA1: \
    case REPOKEY_TYPE_SHA224: \
    case REPOKEY_TYPE_SHA256: \
    case REPOKEY_TYPE_SHA384: \
    case REPOKEY_TYPE_SHA512

#endif

#ifdef __cplusplus
}
#endif

#endif /* LIBSOLV_CHKSUM_H */
