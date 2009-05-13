/*
 * Copyright (c) 2007-2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * transaction.h
 *
 */

#ifndef SATSOLVER_TRANSACTION_H
#define SATSOLVER_TRANSACTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "pooltypes.h"
#include "queue.h"

struct _Solver;

#define SOLVER_TRANSACTION_ERASE		0x10
#define SOLVER_TRANSACTION_REINSTALLED		0x11
#define SOLVER_TRANSACTION_DOWNGRADED		0x12
#define SOLVER_TRANSACTION_CHANGED		0x13
#define SOLVER_TRANSACTION_UPGRADED		0x14
#define SOLVER_TRANSACTION_REPLACED		0x15

#define SOLVER_TRANSACTION_INSTALL		0x20
#define SOLVER_TRANSACTION_REINSTALL		0x21
#define SOLVER_TRANSACTION_DOWNGRADE		0x22
#define SOLVER_TRANSACTION_CHANGE		0x23
#define SOLVER_TRANSACTION_UPGRADE		0x24
#define SOLVER_TRANSACTION_REPLACE		0x25

#define SOLVER_TRANSACTION_MULTIINSTALL		0x30
#define SOLVER_TRANSACTION_MULTIREINSTALL	0x31

#define SOLVER_TRANSACTION_SHOW_ACTIVE          (1 << 0)
#define SOLVER_TRANSACTION_SHOW_ALL             (1 << 1)
#define SOLVER_TRANSACTION_SHOW_REPLACES        (1 << 2)

extern void solver_create_transaction(struct _Solver *solv);
extern void solver_transaction_all_pkgs(struct _Solver *solv, Id p, Queue *pkgs);
extern Id   solver_transaction_pkg(struct _Solver *solv, Id p);
extern Id   solver_transaction_filter(struct _Solver *solv, Id type, Id p, int mode);

#ifdef __cplusplus
}
#endif

#endif
