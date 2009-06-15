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
struct _TransactionOrderdata;

typedef struct _Transaction {
  struct _Pool *pool;		/* back pointer to pool */

  Queue steps;			/* the transaction steps */

  Queue transaction_info;
  Id *transaction_installed;
  Map transactsmap;
  Map noobsmap;

  struct _TransactionOrderdata *orderdata;

} Transaction;


/* step types */
#define SOLVER_TRANSACTION_IGNORE		0x00

#define SOLVER_TRANSACTION_ERASE		0x10
#define SOLVER_TRANSACTION_REINSTALLED		0x11
#define SOLVER_TRANSACTION_DOWNGRADED		0x12
#define SOLVER_TRANSACTION_CHANGED		0x13
#define SOLVER_TRANSACTION_UPGRADED		0x14
#define SOLVER_TRANSACTION_OBSOLETED		0x15

#define SOLVER_TRANSACTION_INSTALL		0x20
#define SOLVER_TRANSACTION_REINSTALL		0x21
#define SOLVER_TRANSACTION_DOWNGRADE		0x22
#define SOLVER_TRANSACTION_CHANGE		0x23
#define SOLVER_TRANSACTION_UPGRADE		0x24
#define SOLVER_TRANSACTION_OBSOLETES		0x25

#define SOLVER_TRANSACTION_MULTIINSTALL		0x30
#define SOLVER_TRANSACTION_MULTIREINSTALL	0x31

#define SOLVER_TRANSACTION_MAXTYPE		0x3f

/* show modes */
#define SOLVER_TRANSACTION_SHOW_ACTIVE		(1 << 0)
#define SOLVER_TRANSACTION_SHOW_ALL		(1 << 1)
#define SOLVER_TRANSACTION_SHOW_OBSOLETES	(1 << 2)
#define SOLVER_TRANSACTION_SHOW_MULTIINSTALL	(1 << 3)
#define SOLVER_TRANSACTION_CHANGE_IS_REINSTALL	(1 << 4)
#define SOLVER_TRANSACTION_MERGE_VENDORCHANGES	(1 << 5)
#define SOLVER_TRANSACTION_MERGE_ARCHCHANGES	(1 << 6)

#define SOLVER_TRANSACTION_RPM_ONLY		(1 << 7)

/* extra classifications */
#define SOLVER_TRANSACTION_ARCHCHANGE		0x100
#define SOLVER_TRANSACTION_VENDORCHANGE		0x101

extern void transaction_init(Transaction *trans, struct _Pool *pool);
extern void transaction_free(Transaction *trans);
extern void transaction_calculate(Transaction *trans, Queue *decisionq, Map *noobsmap);
extern void transaction_all_obs_pkgs(Transaction *trans, Id p, Queue *pkgs);
extern Id   transaction_obs_pkg(Transaction *trans, Id p);
extern Id   transaction_type(Transaction *trans, Id p, int mode);
extern void transaction_classify(Transaction *trans, Queue *classes, int mode);
extern void transaction_classify_pkgs(Transaction *trans, Queue *pkgs, int mode, Id class, Id from, Id to);

extern void transaction_order(Transaction *trans, int flags);
extern int  transaction_order_add_choices(Transaction *trans, Queue *choices, Id chosen);
extern void transaction_check_order(Transaction *trans);

/* order flags */
#define SOLVER_TRANSACTION_KEEP_ORDERDATA	(1 << 0)

#ifdef __cplusplus
}
#endif

#endif
