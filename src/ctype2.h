/*
 * Copyright (c) 2011, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * ctype2.h
 *
 */

#ifndef SATSOLVER_CTYPE2_H
#define SATSOLVER_CTYPE2_H
#define isdigit2(c) ((c)>='0' && (c)<='9')
#define islower2(c) ((c)>='a' && (c)<='z')
#define isupper2(c) ((c)>='A' && (c)<='Z')
#define isalpha2(c) (islower2(c) || isupper2(c))
#define isalnum2(c) (islower2(c) || isupper2(c) || isdigit2(c))
#endif
