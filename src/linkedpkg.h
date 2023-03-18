/*
 * Copyright (c) 2013, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * linkedpkg.h (internal)
 */

#ifndef LIBSOLV_LINKEDPKG_H
#define LIBSOLV_LINKEDPKG_H

static inline int
has_package_link(Pool *pool, Solvable *s)
{
  const char *name = pool_id2str(pool, s->name);
  if (name[0] == 'a' && !strncmp("application:", name, 12))
    return 1;
  if (name[0] == 'p' && !strncmp("pattern:", name, 8))
    return 1;
  if (name[0] == 'p' && !strncmp("product:", name, 8))
    return 1;
  return 0;
}

extern Id find_autopackage_name(Pool *pool, Solvable *s);

/* generic */
extern void find_package_link(Pool *pool, Solvable *s, Id *reqidp, Queue *qr, Id *prvidp, Queue *qp);
extern int pool_link_evrcmp(Pool *pool, Solvable *s1, Solvable *s2);
extern void extend_updatemap_to_buddies(Solver *solv);

#endif
