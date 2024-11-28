/*
 * Copyright (c) 2024, SUSE LLC
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * apk.h
 *
 */

#ifndef LIBSOLV_APK_H
#define LIBSOLV_APK_H

#include "pooltypes.h"

#ifdef __cplusplus
extern "C" {
#endif

int solv_vercmp_apk(const char *evr1, const char *evr1e, const char *evr2, const char *evr2e);
int pool_evrcmp_apk(const Pool *pool, const char *evr1, const char *evr2, int mode);

#ifdef __cplusplus
}
#endif

#endif /* LIBSOLV_APK_H */

