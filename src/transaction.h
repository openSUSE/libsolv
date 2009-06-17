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

/* modes */
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

/* order flags */
#define SOLVER_TRANSACTION_KEEP_ORDERDATA	(1 << 0)

extern void transaction_init(Transaction *trans, struct _Pool *pool);
extern void transaction_init_clone(Transaction *trans, Transaction *srctrans);
extern void transaction_free(Transaction *trans);
extern void transaction_free_orderdata(Transaction *trans);
extern void transaction_calculate(Transaction *trans, Queue *decisionq, Map *noobsmap);

/* if p is installed, returns with pkg(s) obsolete p */
/* if p is not installed, returns with pkg(s) we obsolete */
extern Id   transaction_obs_pkg(Transaction *trans, Id p);
extern void transaction_all_obs_pkgs(Transaction *trans, Id p, Queue *pkgs);

/* return step type of a transaction element */
extern Id   transaction_type(Transaction *trans, Id p, int mode);

/* return sorted collection of all step types */
/* classify_pkgs can be used to return all packages of a type */
extern void transaction_classify(Transaction *trans, int mode, Queue *classes);
extern void transaction_classify_pkgs(Transaction *trans, int mode, Id type, Id from, Id to, Queue *pkgs);

/* order a transaction */
extern void transaction_order(Transaction *trans, int flags);

/* roll your own order funcion: 
 * add pkgs free for installation to queue choices after chosen was
 * installed. start with chosen = 0
 * needs an ordered transaction created with SOLVER_TRANSACTION_KEEP_ORDERDATA */
extern int  transaction_order_add_choices(Transaction *trans, Id chosen, Queue *choices);
/* add obsoleted packages into transaction steps */
extern void transaction_add_obsoleted(Transaction *trans);

/* debug function, report problems found in the order */
extern void transaction_check_order(Transaction *trans);


#ifdef __cplusplus
}
#endif

#endif
