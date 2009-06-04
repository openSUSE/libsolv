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
#include "bitmap.h"

struct _Pool;

typedef struct _Transaction {
  struct _Pool *pool;
  Queue steps;
  Queue transaction_info;
  Id *transaction_installed;
  Map transactsmap;
} Transaction;


/* step types */
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

/* show modes */
#define SOLVER_TRANSACTION_SHOW_ACTIVE          (1 << 0)
#define SOLVER_TRANSACTION_SHOW_ALL             (1 << 1)
#define SOLVER_TRANSACTION_SHOW_REPLACES        (1 << 2)

extern void transaction_init(Transaction *trans, struct _Pool *pool);
extern void transaction_free(Transaction *trans);
extern void transaction_calculate(Transaction *trans, Queue *decisionq, Map *noobsmap);
extern void solver_transaction_all_pkgs(Transaction *trans, Id p, Queue *pkgs);
extern Id   solver_transaction_pkg(Transaction *trans, Id p);
extern Id   solver_transaction_show(Transaction *trans, Id type, Id p, int mode);
extern void transaction_order(Transaction *trans);

#ifdef __cplusplus
}
#endif

#endif
