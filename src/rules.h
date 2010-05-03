/*
 * Copyright (c) 2007-2009, Novell Inc.
 *
 * This program is licensed under the BSD license, read LICENSE.BSD
 * for further information
 */

/*
 * rules.h
 *
 */

#ifndef SATSOLVER_RULES_H
#define SATSOLVER_RULES_H

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------
 * Rule
 *
 *   providerN(B) == Package Id of package providing tag B
 *   N = 1, 2, 3, in case of multiple providers
 *
 * A requires B : !A | provider1(B) | provider2(B)
 *
 * A conflicts B : (!A | !provider1(B)) & (!A | !provider2(B)) ...
 *
 * 'not' is encoded as a negative Id
 *
 * Binary rule: p = first literal, d = 0, w2 = second literal, w1 = p
 *
 * There are a lot of rules, so the struct is kept as small as
 * possible. Do not add new members unless there is no other way.
 */

typedef struct _Rule {
  Id p;		/* first literal in rule */
  Id d;		/* Id offset into 'list of providers terminated by 0' as used by whatprovides; pool->whatprovides + d */
		/* in case of binary rules, d == 0, w1 == p, w2 == other literal */
		/* in case of disabled rules: ~d, aka -d - 1 */
  Id w1, w2;	/* watches, literals not-yet-decided */
		/* if !w2, assertion, not rule */
  Id n1, n2;	/* next rules in linked list, corresponding to w1, w2 */
} Rule;


typedef enum {
  SOLVER_RULE_UNKNOWN = 0,
  SOLVER_RULE_RPM = 0x100,
  SOLVER_RULE_RPM_NOT_INSTALLABLE,
  SOLVER_RULE_RPM_NOTHING_PROVIDES_DEP,
  SOLVER_RULE_RPM_PACKAGE_REQUIRES,
  SOLVER_RULE_RPM_SELF_CONFLICT,
  SOLVER_RULE_RPM_PACKAGE_CONFLICT,
  SOLVER_RULE_RPM_SAME_NAME,
  SOLVER_RULE_RPM_PACKAGE_OBSOLETES,
  SOLVER_RULE_RPM_IMPLICIT_OBSOLETES,
  SOLVER_RULE_RPM_INSTALLEDPKG_OBSOLETES,
  SOLVER_RULE_UPDATE = 0x200,
  SOLVER_RULE_FEATURE = 0x300,
  SOLVER_RULE_JOB = 0x400,
  SOLVER_RULE_JOB_NOTHING_PROVIDES_DEP,
  SOLVER_RULE_DISTUPGRADE = 0x500,
  SOLVER_RULE_INFARCH = 0x600,
  SOLVER_RULE_CHOICE = 0x700,
  SOLVER_RULE_LEARNT = 0x800
} SolverRuleinfo;

#define SOLVER_RULE_TYPEMASK    0xff00

struct _Solver;

/*-------------------------------------------------------------------
 * disable rule
 */

static inline void
solver_disablerule(struct _Solver *solv, Rule *r)
{
  if (r->d >= 0)
    r->d = -r->d - 1;
}

/*-------------------------------------------------------------------
 * enable rule
 */

static inline void
solver_enablerule(struct _Solver *solv, Rule *r)
{
  if (r->d < 0)
    r->d = -r->d - 1;
}

Rule *solver_addrule(struct _Solver *solv, Id p, Id d);
void solver_unifyrules(struct _Solver *solv);
int solver_samerule(struct _Solver *solv, Rule *r1, Rule *r2);

/* rpm rules */
void solver_addrpmrulesforsolvable(struct _Solver *solv, Solvable *s, Map *m);
void solver_addrpmrulesforweak(struct _Solver *solv, Map *m);
void solver_addrpmrulesforupdaters(struct _Solver *solv, Solvable *s, Map *m, int allow_all);

/* update/feature rules */
void solver_addupdaterule(struct _Solver *solv, Solvable *s, int allow_all);

/* infarch rules */
void solver_addinfarchrules(struct _Solver *solv, Map *addedmap);

/* dup rules */
void solver_createdupmaps(struct _Solver *solv);
void solver_freedupmaps(struct _Solver *solv);
void solver_addduprules(struct _Solver *solv, Map *addedmap);

/* policy rule disabling/reenabling */
void solver_disablepolicyrules(struct _Solver *solv);
void solver_reenablepolicyrules(struct _Solver *solv, int jobidx);

/* rule info */
int solver_allruleinfos(struct _Solver *solv, Id rid, Queue *rq);
SolverRuleinfo solver_ruleinfo(struct _Solver *solv, Id rid, Id *fromp, Id *top, Id *depp);

/* misc functions */
void solver_disablechoicerules(struct _Solver *solv, Rule *r);

#ifdef __cplusplus
}
#endif

#endif

