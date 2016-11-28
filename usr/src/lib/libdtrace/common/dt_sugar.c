/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2012, 2016 by Delphix. All rights reserved.
 */

/*
 * Syntactic sugar features are implemented by transforming the D parse tree
 * such that it only uses the subset of D that is supported by the rest of the
 * compiler / the kernel.  A clause containing these language features is
 * referred to as a "super-clause", and its transformation typically entails
 * creating several "sub-clauses" to implement it. For diagnosability, the
 * sub-clauses will be printed if the "-xtree=8" flag is specified.
 *
 * The features are:
 *
 * "if/else" statements.  Each basic block (e.g. the body of the "if"
 * and "else" statements, and the statements before and after) is turned
 * into its own sub-clause, with a predicate that causes it to be
 * executed only if the code flows to this point.  Nested if/else
 * statements are supported.
 *
 * "while<N>" statements.  The while loop is unrolled by creating N
 * copies of the loop control condition and body.
 *
 * "entry->" variables.  These variables are valid only in :return
 * probes, and reference state (e.g. args[], timestamp) from the
 * corresponding :entry probes.  A sub-clause will be created to record
 * the state from the corresponding :entry probe.  Note that clauses
 * containing entry-> variables will only be executed if the
 * corresponding :entry probe executed.
 *
 * "callers[]" variables.  These variables count the number of times a
 * given function (or set of functions) appears in the call stack, based
 * on firing of the specified :entry and :return probes.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>

#include <assert.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <dt_module.h>
#include <dt_program.h>
#include <dt_provider.h>
#include <dt_printf.h>
#include <dt_pid.h>
#include <dt_grammar.h>
#include <dt_ident.h>
#include <dt_string.h>
#include <dt_impl.h>

typedef enum entryvar {
	ENTRYVAR_VTIMESTAMP,
	ENTRYVAR_WALLTIMESTAMP,
	ENTRYVAR_ERRNO,
	ENTRYVAR_ELAPSED_NS,
	ENTRYVAR_ELAPSED_US,
	ENTRYVAR_ELAPSED_MS,
	ENTRYVAR_ELAPSED_SEC,
	ENTRYVAR_ARG0,
	ENTRYVAR_ARG1,
	ENTRYVAR_ARG2,
	ENTRYVAR_ARG3,
	ENTRYVAR_ARG4,
	ENTRYVAR_ARG5,
	ENTRYVAR_ARG6,
	ENTRYVAR_ARG7,
	ENTRYVAR_ARG8,
	ENTRYVAR_ARG9,
	ENTRYVAR_ARGS0,
	ENTRYVAR_ARGS1,
	ENTRYVAR_ARGS2,
	ENTRYVAR_ARGS3,
	ENTRYVAR_ARGS4,
	ENTRYVAR_ARGS5,
	ENTRYVAR_ARGS6,
	ENTRYVAR_ARGS7,
	ENTRYVAR_ARGS8,
	ENTRYVAR_ARGS9,
	ENTRYVAR_TIMESTAMP,
	ENTRYVAR_NUM
} entryvar_t;

#define	ENTRYVAR_IS_ARGS (1<<0)
#define	ENTRYVAR_IS_ELAPSED (1<<1)

typedef struct dt_sugar_entryvar {
	const char *xe_name;
	int xe_flags;
	int xe_num;
} dt_sugar_entryvar_t;

static const dt_sugar_entryvar_t entryvars[] = {
	{ "vtimestamp" },
	{ "walltimestamp" },
	{ "errno" },
	{ "elapsed_ns", ENTRYVAR_IS_ELAPSED, 1 },
	{ "elapsed_us", ENTRYVAR_IS_ELAPSED, 1000 },
	{ "elapsed_ms", ENTRYVAR_IS_ELAPSED, 1000 * 1000 },
	{ "elapsed_sec", ENTRYVAR_IS_ELAPSED, 1000 * 1000 * 1000 },
	{ "arg0", 0, 0 },
	{ "arg1", 0, 1 },
	{ "arg2", 0, 2 },
	{ "arg3", 0, 3 },
	{ "arg4", 0, 4 },
	{ "arg5", 0, 5 },
	{ "arg6", 0, 6 },
	{ "arg7", 0, 7 },
	{ "arg8", 0, 8 },
	{ "arg9", 0, 9 },
	{ "args0", ENTRYVAR_IS_ARGS, 0 },
	{ "args1", ENTRYVAR_IS_ARGS, 1 },
	{ "args2", ENTRYVAR_IS_ARGS, 2 },
	{ "args3", ENTRYVAR_IS_ARGS, 3 },
	{ "args4", ENTRYVAR_IS_ARGS, 4 },
	{ "args5", ENTRYVAR_IS_ARGS, 5 },
	{ "args6", ENTRYVAR_IS_ARGS, 6 },
	{ "args7", ENTRYVAR_IS_ARGS, 7 },
	{ "args8", ENTRYVAR_IS_ARGS, 8 },
	{ "args9", ENTRYVAR_IS_ARGS, 9 },
	/*
	 * Note! timestamp must come last, because it is the variable
	 * that we check to ensure that all entry variables were set (i.e.
	 * there were no drops).  If it was not last, it could succeed but
	 * then we could drop the subsequent variables.
	 */
	{ "timestamp" },
	{ NULL }
};

typedef struct dt_sugar_parse {
	dtrace_hdl_t *dtsp_dtp;		/* dtrace handle */
	dt_node_t *dtsp_pdescs;		/* probe descriptions */
	dt_node_t *dtsp_append_clauses;	/* callers return clauses to append */
	int dtsp_num_conditions;	/* number of condition variables */
	int dtsp_num_ifs;		/* number of "if" statements */
	int dtsp_num_whiles;		/* number of "while" statements */
	dt_node_t *dtsp_clause_list;	/* list of clauses */
	boolean_t dtsp_need_entry;
	boolean_t dtsp_entryvars[ENTRYVAR_NUM];
	boolean_t dtsp_in_return;
} dt_sugar_parse_t;

static void dt_sugar_visit_stmts(dt_sugar_parse_t *, dt_node_t *, int);

/*
 * Return a node for "self->%error".
 *
 * Note that the "%" is part of the variable name, and is included so that
 * this variable name can not collide with any user-specified variable.
 *
 * This error variable is used to keep track of if there has been an error
 * in any of the sub-clauses, and is used to prevent execution of subsequent
 * sub-clauses following an error.
 */
static dt_node_t *
dt_sugar_new_error_var(void)
{
	return (dt_node_op2(DT_TOK_PTR, dt_node_ident(strdup("self")),
	    dt_node_ident(strdup("%error"))));
}

/*
 * Append this clause to the clause list.
 */
static void
dt_sugar_append_clause(dt_sugar_parse_t *dp, dt_node_t *clause)
{
	dp->dtsp_clause_list = dt_node_link(dp->dtsp_clause_list, clause);
}

/*
 * Prepend this clause to the clause list.
 */
static void
dt_sugar_prepend_clause(dt_sugar_parse_t *dp, dt_node_t *clause)
{
	dp->dtsp_clause_list = dt_node_link(clause, dp->dtsp_clause_list);
}

/*
 * Return a node for "this->%condition_<condid>", or NULL if condid==0.
 *
 * Note that the "%" is part of the variable name, and is included so that
 * this variable name can not collide with any user-specified variable.
 */
static dt_node_t *
dt_sugar_new_condition_var(int condid)
{
	char *str;

	if (condid == 0)
		return (NULL);
	assert(condid > 0);

	(void) asprintf(&str, "%%condition_%d", ABS(condid));
	return (dt_node_op2(DT_TOK_PTR, dt_node_ident(strdup("this")),
	    dt_node_ident(str)));
}

/*
 * Return new clause to evaluate predicate and set newcond.  condid is
 * the condition that we are already under, or 0 if none.
 * The new clause will be of the form:
 *
 * dp_pdescs
 * /!self->%error/
 * {
 *	this->%condition_<newcond> =
 *	    (this->%condition_<condid> && pred);
 * }
 *
 * Note: if condid==0, we will instead do "... = (1 && pred)", to effectively
 * convert the pred to a boolean.
 *
 * Note: Unless an error has been encountered, we always set the condition
 * variable (either to 0 or 1).  This lets us avoid resetting the condition
 * variables back to 0 when the super-clause completes.
 */
static dt_node_t *
dt_sugar_new_condition_impl(dt_sugar_parse_t *dp,
    dt_node_t *pred, int condid, int newcond)
{
	dt_node_t *value, *body, *newpred;

	/* predicate is !self->%error */
	newpred = dt_node_op1(DT_TOK_LNEG, dt_sugar_new_error_var());

	if (condid == 0) {
		/*
		 * value is (1 && pred)
		 *
		 * Note, D doesn't allow a probe-local "this" variable to
		 * be reused as a different type, even from a different probe.
		 * Therefore, value can't simply be <pred>, because then
		 * its type could be different when we reuse this condid
		 * in a different meta-clause.
		 */
		value = dt_node_op2(DT_TOK_LAND, dt_node_int(1), pred);
	} else {
		/* value is (this->%condition_<condid> && pred) */
		value = dt_node_op2(DT_TOK_LAND,
		    dt_sugar_new_condition_var(condid), pred);
	}

	/* body is "this->%condition_<retval> = <value>;" */
	body = dt_node_statement(dt_node_op2(DT_TOK_ASGN,
	    dt_sugar_new_condition_var(newcond), value));

	return (dt_node_clause(dp->dtsp_pdescs, newpred, body));
}

/*
 * Generate a new clause to evaluate predicate and set a new condition variable,
 * whose ID will be returned.  The new clause will be appended to
 * dp_first_new_clause.
 */
static int
dt_sugar_new_condition(dt_sugar_parse_t *dp, dt_node_t *pred, int condid)
{
	dp->dtsp_num_conditions++;
	dt_sugar_append_clause(dp, dt_sugar_new_condition_impl(dp,
	    pred, condid, dp->dtsp_num_conditions));
	return (dp->dtsp_num_conditions);
}

/*
 * Return a node representing:
 *   self->%entry_<de_name>_<dt_sugar_num_entrys>[stackdepth]
 *
 * Note that we need to include dt_sugar_num_entrys so that uses of
 * entry->args[N] will have different names when used from different
 * super-clauses.  This is necessary because a given variable can only
 * have one type throughout the entire program (i.e. across different
 * super-clauses).
 */
static dt_node_t *
dt_sugar_new_entry_var(dt_sugar_parse_t *dp, entryvar_t ev)
{
	dt_node_t *self_entry, *identlist;
	char *str;

	(void) asprintf(&str, "%%entry_%s_%u",
	    entryvars[ev].xe_name, dp->dtsp_dtp->dt_sugar_num_entrys);

	self_entry = dt_node_op2(DT_TOK_PTR, dt_node_ident(strdup("self")),
	    dt_node_ident(str));

	identlist = dt_node_ident(strdup("stackdepth"));

	return (dt_node_op2(DT_TOK_LBRAC, self_entry, identlist));
}

/*
 * Return a clause which sets the entry-> variables from the :entry probes
 * corresponding to the current dp_pdescs (which must be :return probes).
 *
 * <dp_pdescs with dtpd_name changed to "entry">
 * {
 *	// for each entryvar_t ev:
 *	//   if dp_entryvars[ev], generate
 *	self->%entry_<name>[stackdepth] = <name>;
 *	//   special case for entry->args[N]:
 *	self->%entry_argsN[stackdepth] = args[N];
 *
 *	// always included as the last statement:
 *	self->%entry_timestamp[stackdepth] = timestamp;
 * }
 *
 * Note that we always set self->%entry_timestamp[stackdepth], because
 * it is used to indicate that the :entry clause was successfully executed,
 * and thus the :return clause can be safely executed.  The timestamp
 * comes last so that we know that all other %entry_* variables were set
 * if it is also set (i.e. we didn't encounter an error while setting any
 * entry variable).
 */
static dt_node_t *
dt_sugar_new_entry_clause(dt_sugar_parse_t *dp)
{
	dt_node_t *pdesc;
	dt_node_t *newpdesc = NULL;
	dt_node_t *stmts = NULL;
	entryvar_t ev;

	assert(dp->dtsp_entryvars[ENTRYVAR_TIMESTAMP]);
	for (ev = 0; ev < ENTRYVAR_NUM; ev++) {
		dt_node_t *val, *asgn;

		if (!dp->dtsp_entryvars[ev] ||
		    (entryvars[ev].xe_flags & ENTRYVAR_IS_ELAPSED))
			continue;

		if (entryvars[ev].xe_flags & ENTRYVAR_IS_ARGS) {
			val = dt_node_op2(DT_TOK_LBRAC,
			    dt_node_ident(strdup("args")),
			    dt_node_int(entryvars[ev].xe_num));
		} else {
			val = dt_node_ident(strdup(entryvars[ev].xe_name));
		}
		asgn = dt_node_op2(DT_TOK_ASGN,
		    dt_sugar_new_entry_var(dp, ev), val);
		stmts = dt_node_link(stmts, dt_node_statement(asgn));
	}

	for (pdesc = dp->dtsp_pdescs; pdesc != NULL; pdesc = pdesc->dn_list) {
		char *probename;
		assert(strcmp(pdesc->dn_desc->dtpd_name, "return") == 0);
		(void) asprintf(&probename, "%s:%s:%s:entry",
		    pdesc->dn_desc->dtpd_provider,
		    pdesc->dn_desc->dtpd_mod,
		    pdesc->dn_desc->dtpd_func);
		newpdesc = dt_node_link(newpdesc,
		    dt_node_pdesc_by_name(probename));
	}

	return (dt_node_clause(newpdesc, NULL, stmts));
}

/*
 * Return a clause which clears all of the self->%entry_* variables
 * that were set by the :entry clause generated by new_entry_clause().
 *
 * dp_pdescs
 * {
 *	// for each entryvar_t ev:
 *	//   if dp_entryvars[ev], generate
 *	self->%entry_<name>[stackdepth] = 0;
 *
 *	// always included:
 *	self->%entry_timestamp[stackdepth] = 0;
 * }
 */
static dt_node_t *
dt_sugar_new_return_clause(dt_sugar_parse_t *dp)
{
	dt_node_t *stmts = NULL;
	entryvar_t ev;

	assert(dp->dtsp_entryvars[ENTRYVAR_TIMESTAMP]);
	for (ev = 0; ev < ENTRYVAR_NUM; ev++) {
		dt_node_t *asgn;

		if (!dp->dtsp_entryvars[ev])
			continue;

		asgn = dt_node_op2(DT_TOK_ASGN,
		    dt_sugar_new_entry_var(dp, ev), dt_node_int(0));
		stmts = dt_node_link(stmts, dt_node_statement(asgn));
	}

	return (dt_node_clause(dp->dtsp_pdescs, NULL, stmts));
}

/*
 * Return a new clause which sets the first condition variable if
 * the :entry clause which saved the %entry_* variables was successfully
 * executed.
 *
 * dp_pdescs
 * /self->%entry_timestamp[stackdepth]/
 * {
 *	this->%condition_1 = 1;
 * }
 */
static dt_node_t *
dt_sugar_new_condition1_clause(dt_sugar_parse_t *dp)
{
	dt_node_t *pred = dt_sugar_new_entry_var(dp, ENTRYVAR_TIMESTAMP);
	return (dt_sugar_new_condition_impl(dp, pred, 0, 1));
}

/*
 * Replace "old" with "new", and remember to generate the :entry and :return
 * probes to record and reset the entry variables.
 *
 * See also new_entry_var(), new_entry_clause(), and new_return_clause().
 */
static void
dt_sugar_do_entryvar_impl(dt_sugar_parse_t *dp, entryvar_t ev,
    dt_node_t *old, dt_node_t *new)
{
	dt_node_t *link = old->dn_link;
	dt_node_t *list = old->dn_list;

	*old = *new;
	old->dn_link = link;
	old->dn_list = list;
	new->dn_kind = DT_NODE_FREE;

	/* remember to generate :entry and :return probes */
	dp->dtsp_need_entry = B_TRUE;
	dp->dtsp_entryvars[ev] = B_TRUE;
	/* We must always set %entry_timestamp<N>; see new_entry_clause() */
	dp->dtsp_entryvars[ENTRYVAR_TIMESTAMP] = B_TRUE;
}

/*
 * If we are in a clause whose probes are all :return probes, and this node is
 * a bare entry variable (i.e. "entry->timestamp", "entry->arg1",
 * "entry->elapsed_ms", etc -- but not entry->args[N], which is handled by
 * dt_sugar_do_entryvar_args()), replace it with the corresponding straight D
 * nodes.  Also, remember that we need to generate the :entry clause to
 * record this entry variable, and the extra :return clause to clear it.
 *
 * This node is transformed to either:
 *   self->%entry_<name>_<dt_sugar_num_entrys>[stackdepth]
 * or, for entry_elapsed_*:
 *   (timestamp - self->%entry_timestamp_<dt_sugar_num_entrys>[stackdepth]) /
 *   <de_num>
 * e.g, "entry_elapsed_us" could become:
 *   (timestamp - self->%entry_timestamp2[stackdepth] / 1000)
 *
 * See also new_entry_var(), new_entry_clause(), and new_return_clause().
 */
static void
dt_sugar_do_entryvar(dt_sugar_parse_t *dp, dt_node_t *dnp)
{
	dt_node_t *n;
	entryvar_t ev;

	if (!dp->dtsp_in_return)
		return;
	if (dnp->dn_kind != DT_NODE_OP2 ||
	    dnp->dn_op != DT_TOK_PTR)
		return;
	if (dnp->dn_left->dn_kind != DT_NODE_IDENT ||
	    strcmp(dnp->dn_left->dn_string, "entry") != 0)
		return;
	if (dnp->dn_right->dn_kind != DT_NODE_IDENT)
		return;

	for (ev = 0; ev < ENTRYVAR_NUM; ev++) {
		if (strcmp(dnp->dn_right->dn_string,
		    entryvars[ev].xe_name) == 0)
			break;
	}
	if (ev == ENTRYVAR_NUM)
		return;

	/*
	 * If this is the first time we are using an entry_* variable in
	 * this super-clause, increment dt_sugar_num_entrys so that our
	 * %entry_timestamp<N> variables will be different from
	 * that of other super-clauses.  See also new_entry_var().
	 */
	if (!dp->dtsp_need_entry)
		dp->dtsp_dtp->dt_sugar_num_entrys++;

	if (entryvars[ev].xe_flags & ENTRYVAR_IS_ELAPSED) {
		/*
		 * The value is: (timestamp -
		 *   self->%entry_timestamp[stackdepth]) / <de_num>
		 */
		dt_node_t *delta_ns = dt_node_op2(DT_TOK_SUB,
		    dt_node_ident(strdup("timestamp")),
		    dt_sugar_new_entry_var(dp, ENTRYVAR_TIMESTAMP));
		n = dt_node_op2(DT_TOK_DIV, delta_ns,
		    dt_node_int(entryvars[ev].xe_num));
		/*
		 * Treat entry_elapsed_* like entry_timestamp for purposes of
		 * entry/return probes.
		 */
		ev = ENTRYVAR_TIMESTAMP;
	} else {
		n = dt_sugar_new_entry_var(dp, ev);
	}

	dt_sugar_do_entryvar_impl(dp, ev, dnp, n);
}

/*
 * If we are in a clause whose probes are all :return probes, and this node is
 * is "entry->args[N]", replace it with the corresponding straight D nodes.
 * Also, remember that we need to generate the :entry clause to
 * record this entry variable, and the extra :return clause to clear it.
 * Note that the N in "entry->args[N]" must be a single-digit literal integer,
 * not a variable.
 *
 * This node is transformed to:
 *   self->%entry_args<N>_<dt_sugar_num_entrys>[stackdepth]
 *
 * See also new_entry_var(), new_entry_clause(), and new_return_clause().
 */
static void
dt_sugar_do_entryvar_args(dt_sugar_parse_t *dp, dt_node_t *dnp)
{
	entryvar_t ev;

	if (!dp->dtsp_in_return)
		return;

	if (dnp->dn_kind != DT_NODE_OP2 ||
	    dnp->dn_op != DT_TOK_LBRAC)
		return;
	if (dnp->dn_left->dn_kind != DT_NODE_OP2 ||
	    dnp->dn_left->dn_op != DT_TOK_PTR)
		return;
	if (dnp->dn_left->dn_left->dn_kind != DT_NODE_IDENT ||
	    strcmp(dnp->dn_left->dn_left->dn_string, "entry") != 0)
		return;
	if (dnp->dn_left->dn_right->dn_kind != DT_NODE_IDENT ||
	    strcmp(dnp->dn_left->dn_right->dn_string, "args") != 0)
		return;
	if (dnp->dn_right->dn_kind != DT_NODE_INT ||
	    dnp->dn_right->dn_value > 9)
		return;

	/* See comment in dt_sugar_do_entryvar(). */
	if (!dp->dtsp_need_entry)
		dp->dtsp_dtp->dt_sugar_num_entrys++;

	ev = ENTRYVAR_ARGS0 + dnp->dn_right->dn_value;
	dt_sugar_do_entryvar_impl(dp, ev, dnp, dt_sugar_new_entry_var(dp, ev));
}

/*
 * Return a node representing:
 * self->%callers_<ID>
 */
static dt_node_t *
dt_sugar_new_callers_var(int id)
{
	char *str;

	(void) asprintf(&str, "%%callers_%u", id);
	return (dt_node_op2(DT_TOK_PTR, dt_node_ident(strdup("self")),
	    dt_node_ident(str)));
}

/*
 * If this node is a callers variable (i.e. callers["probedesc"]), replace
 * it with the implementation:
 *   self->%callers_<ID>
 *
 * Also, generate the new :entry and :return clauses to increment/decrement
 * self->%callers_<ID>.  Prepend the :entry clause to dp_first_new_clause,
 * and remember the :return clause; it will be appended to the very end of
 * the clause list (after any other clauses).
 *
 * The new :entry clause will be:
 *   <probedesc>:entry{ ++self->%callers_<ID>; }
 *
 * The new :return clause will be:
 *   <probedesc>:return/self->%callers_<ID>/{ --self->%callers_<ID>; }
 *
 * Note that the "probedesc" must be a probe description *without* the trailing
 * :entry or :return.  It may be a comma-separated list, e.g.:
 *   "spa_sync,fbt:zfs:dsl_pool_sync"
 */
static void
dt_sugar_do_callers(dt_sugar_parse_t *dp, dt_node_t *dnp)
{
	dt_node_t *link = dnp->dn_link;
	dt_node_t *list = dnp->dn_list;
	dt_node_t *n, *stmt, *clause;
	dt_node_t *entrydesc = NULL;
	dt_node_t *returndesc = NULL;
	char *callers, *token;

	if (dnp->dn_kind != DT_NODE_OP2)
		return;
	if (dnp->dn_op != DT_TOK_LBRAC)
		return;
	if (dnp->dn_left->dn_kind != DT_NODE_IDENT ||
	    strcmp(dnp->dn_left->dn_string, "callers") != 0)
		return;
	if (dnp->dn_right->dn_kind != DT_NODE_STRING)
		return;

	dp->dtsp_dtp->dt_sugar_num_callers++;
	n = dt_sugar_new_callers_var(dp->dtsp_dtp->dt_sugar_num_callers);
	callers = dnp->dn_right->dn_string;

	/* replace arg with n */
	*dnp = *n;
	dnp->dn_link = link;
	dnp->dn_list = list;
	n->dn_kind = DT_NODE_FREE;

	/* make new probe descriptions */
	while ((token = strsep(&callers, ",")) != NULL) {
		char *probename;
		(void) asprintf(&probename, "%s:entry", token);
		entrydesc = dt_node_link(entrydesc,
		    dt_node_pdesc_by_name(probename));
		(void) asprintf(&probename, "%s:return", token);
		returndesc = dt_node_link(returndesc,
		    dt_node_pdesc_by_name(probename));
	}

	/* make :entry clause */
	stmt = dt_node_statement(dt_node_op1(DT_TOK_PREINC,
	    dt_sugar_new_callers_var(dp->dtsp_dtp->dt_sugar_num_callers)));
	dt_sugar_prepend_clause(dp, dt_node_clause(entrydesc, NULL, stmt));

	/* make :return clause */
	stmt = dt_node_statement(dt_node_op1(DT_TOK_PREDEC,
	    dt_sugar_new_callers_var(dp->dtsp_dtp->dt_sugar_num_callers)));
	clause = dt_node_clause(returndesc,
	    dt_sugar_new_callers_var(dp->dtsp_dtp->dt_sugar_num_callers), stmt);
	/* remember to append it after all other clauses */
	dp->dtsp_append_clauses = dt_node_link(dp->dtsp_append_clauses, clause);
}

/*
 * Visit the specified node and all of its descendants.  Replace any
 * entry_* variables (e.g. entry_walltimestamp,entry_args[1], entry_elapsed_us)
 * and callers[""] variables (e.g. callers["spa_sync"]) with the corresponding
 * straight-D nodes.
 */
static void
dt_sugar_visit_all(dt_sugar_parse_t *dp, dt_node_t *dnp)
{
	dt_node_t *arg;

	dt_sugar_do_entryvar(dp, dnp);
	dt_sugar_do_entryvar_args(dp, dnp);
	dt_sugar_do_callers(dp, dnp);

	switch (dnp->dn_kind) {
	case DT_NODE_FREE:
	case DT_NODE_INT:
	case DT_NODE_STRING:
	case DT_NODE_SYM:
	case DT_NODE_TYPE:
	case DT_NODE_PROBE:
	case DT_NODE_PDESC:
	case DT_NODE_IDENT:
		break;

	case DT_NODE_FUNC:
		for (arg = dnp->dn_args; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);
		break;

	case DT_NODE_OP1:
		dt_sugar_visit_all(dp, dnp->dn_child);
		break;

	case DT_NODE_OP2:
		dt_sugar_visit_all(dp, dnp->dn_left);
		dt_sugar_visit_all(dp, dnp->dn_right);
		if (dnp->dn_op == DT_TOK_LBRAC) {
			dt_node_t *ln = dnp->dn_right;
			while (ln->dn_list != NULL) {
				dt_sugar_visit_all(dp, ln->dn_list);
				ln = ln->dn_list;
			}
		}
		break;

	case DT_NODE_OP3:
		dt_sugar_visit_all(dp, dnp->dn_expr);
		dt_sugar_visit_all(dp, dnp->dn_left);
		dt_sugar_visit_all(dp, dnp->dn_right);
		break;

	case DT_NODE_DEXPR:
	case DT_NODE_DFUNC:
		dt_sugar_visit_all(dp, dnp->dn_expr);
		break;

	case DT_NODE_AGG:
		for (arg = dnp->dn_aggtup; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);

		if (dnp->dn_aggfun)
			dt_sugar_visit_all(dp, dnp->dn_aggfun);
		break;

	case DT_NODE_CLAUSE:
		for (arg = dnp->dn_pdescs; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);

		if (dnp->dn_pred != NULL)
			dt_sugar_visit_all(dp, dnp->dn_pred);

		for (arg = dnp->dn_acts; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);
		break;

	case DT_NODE_INLINE: {
		const dt_idnode_t *inp = dnp->dn_ident->di_iarg;

		dt_sugar_visit_all(dp, inp->din_root);
		break;
	}
	case DT_NODE_MEMBER:
		if (dnp->dn_membexpr)
			dt_sugar_visit_all(dp, dnp->dn_membexpr);
		break;

	case DT_NODE_XLATOR:
		for (arg = dnp->dn_members; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);
		break;

	case DT_NODE_PROVIDER:
		for (arg = dnp->dn_probes; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);
		break;

	case DT_NODE_PROG:
		for (arg = dnp->dn_list; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);
		break;

	case DT_NODE_IF:
		dp->dtsp_num_ifs++;
		dt_sugar_visit_all(dp, dnp->dn_conditional);

		for (arg = dnp->dn_body; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);
		for (arg = dnp->dn_alternate_body; arg != NULL;
		    arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);

		break;

	case DT_NODE_WHILE:
		dp->dtsp_num_whiles++;
		dt_sugar_visit_all(dp, dnp->dn_conditional);
		for (arg = dnp->dn_body; arg != NULL; arg = arg->dn_list)
			dt_sugar_visit_all(dp, arg);

		break;

	default:
		(void) dnerror(dnp, D_UNKNOWN, "bad node %p, kind %d\n",
		    (void *)dnp, dnp->dn_kind);
	}
}

/*
 * Return a new clause which resets the error variable to zero:
 *
 *   dp_pdescs{ self->%error = 0; }
 *
 * This clause will be executed at the beginning of each meta-clause, to
 * ensure the error variable is unset (in case the previous meta-clause
 * failed).
 */
static dt_node_t *
dt_sugar_new_clearerror_clause(dt_sugar_parse_t *dp)
{
	dt_node_t *stmt = dt_node_statement(dt_node_op2(DT_TOK_ASGN,
	    dt_sugar_new_error_var(), dt_node_int(0)));
	return (dt_node_clause(dp->dtsp_pdescs, NULL, stmt));
}

/*
 * Evaluate the conditional, and recursively visit the body of the "if"
 * statement (and the "else", if present).
 */
static void
dt_sugar_do_if(dt_sugar_parse_t *dp, dt_node_t *if_stmt, int precondition)
{
	int newid;

	assert(if_stmt->dn_kind == DT_NODE_IF);

	/* condition */
	newid = dt_sugar_new_condition(dp,
	    if_stmt->dn_conditional, precondition);

	/* body of if */
	dt_sugar_visit_stmts(dp, if_stmt->dn_body, newid);

	/*
	 * Visit the body of the "else" statement, if present.  Note that we
	 * generate a new condition which is the inverse of the previous
	 * condition.
	 */
	if (if_stmt->dn_alternate_body != NULL) {
		dt_node_t *pred =
		    dt_node_op1(DT_TOK_LNEG, dt_sugar_new_condition_var(newid));
		dt_sugar_visit_stmts(dp, if_stmt->dn_alternate_body,
		    dt_sugar_new_condition(dp, pred, precondition));
	}
}

/*
 * Append first, and all following clauses until (and including) last,
 * to dp_first_new_clause.  This is used by dt_sugar_do_while() to unroll
 * the loop.
 */
static void
dt_sugar_copy_clauses(dt_sugar_parse_t *dp, dt_node_t *first, dt_node_t *last)
{
	dt_node_t *dn;
	for (dn = first; dn != last->dn_list; dn = dn->dn_list) {
		assert(dn->dn_kind == DT_NODE_CLAUSE);
		dt_sugar_append_clause(dp, dt_node_clause(dn->dn_pdescs,
		    dn->dn_pred, dn->dn_acts));
	}
}

/*
 * Handle a "while" statement:
 *   while<N> (<control statement>) { <body>; }
 *
 * Evaluate the control statement, and recursively visit the body of the "while"
 * statement.  Then unroll the loop, creating the specified number of copies
 * of all clauses that are part of the "while" control statement and body.
 *
 * The generated clauses will look like (in this example, notdoneid==1):
 *
 * dp_pdescs
 * /!self->%error/
 * {
 *	this->%condition_1 =
 *	    (this->%condition_<precondition> && 1);
 * }
 *
 * // loop begin:
 *
 * dp_pdescs
 * /!self->%error/
 * {
 *	this->%condition_1 =
 *	    (this->%condition_1 && <control statement>);
 * }
 *
 * dp_pdescs
 * /!self->%error && this->%condition_1/
 * {
 *	<body>;
 * }
 *
 * // loop end; repeat above clauses specified number of times
 */
static void
dt_sugar_do_while(dt_sugar_parse_t *dp, dt_node_t *while_stmt, int precondition)
{
	int i, notdoneid;
	dt_node_t *loop_first, *loop_last;

	assert(while_stmt->dn_kind == DT_NODE_WHILE);

	/*
	 * This condition is true until the control statement
	 * evaluates to false.
	 */
	notdoneid = dt_sugar_new_condition(dp, dt_node_int(1), precondition);

	dt_sugar_append_clause(dp, dt_sugar_new_condition_impl(dp,
	    while_stmt->dn_conditional, notdoneid, notdoneid));

	/*
	 * The last node is the one which new_condition() just generated
	 * to evaluate the control statement.  This is the first clause
	 * that will be part of the unrolled loop.
	 */
	loop_first = dt_node_last(dp->dtsp_clause_list);

	/*
	 * Recursively visit the body of the "while" statement.
	 */
	dt_sugar_visit_stmts(dp, while_stmt->dn_body, notdoneid);

	/*
	 * The last node from the body is the last one that needs to be copied.
	 */
	loop_last = dt_node_last(loop_first);

	/*
	 * Create the specified number of copies of the clauses which
	 * evaluate the control statement and the body.
	 */
	for (i = 1; i < while_stmt->dn_max_iter; i++)
		dt_sugar_copy_clauses(dp, loop_first, loop_last);
}

/*
 * Generate a new clause to evaluate the statements based on the condition.
 * The new clause will be appended to dp_first_new_clause.
 *
 * dp_pdescs
 * /!self->%error && this->%condition_<condid>/
 * {
 *	stmts
 * }
 */
static void
dt_sugar_new_basic_block(dt_sugar_parse_t *dp, int condid, dt_node_t *stmts)
{
	dt_node_t *pred = NULL;

	if (condid == 0) {
		/*
		 * Don't bother with !error on the first clause, because if
		 * there is only one clause, we don't add the prelude to
		 * zero out %error.
		 */
		if (dp->dtsp_num_conditions != 0) {
			pred = dt_node_op1(DT_TOK_LNEG,
			    dt_sugar_new_error_var());
		}
	} else {
		pred = dt_node_op2(DT_TOK_LAND,
		    dt_node_op1(DT_TOK_LNEG, dt_sugar_new_error_var()),
		    dt_sugar_new_condition_var(condid));
	}
	dt_sugar_append_clause(dp,
	    dt_node_clause(dp->dtsp_pdescs, pred, stmts));
}

/*
 * Visit all the statements in this list, and break them into basic blocks,
 * generating new clauses for "if", "else", and "while" statements.
 */
static void
dt_sugar_visit_stmts(dt_sugar_parse_t *dp, dt_node_t *stmts, int precondition)
{
	dt_node_t *stmt;
	dt_node_t *prev_stmt = NULL;
	dt_node_t *next_stmt;
	dt_node_t *first_stmt_in_basic_block = NULL;

	for (stmt = stmts; stmt != NULL; stmt = next_stmt) {
		next_stmt = stmt->dn_list;

		if (stmt->dn_kind != DT_NODE_IF &&
		    stmt->dn_kind != DT_NODE_WHILE) {
			if (first_stmt_in_basic_block == NULL)
				first_stmt_in_basic_block = stmt;
			prev_stmt = stmt;
			continue;
		}

		/*
		 * Remove this and following statements from the previous
		 * clause.
		 */
		if (prev_stmt != NULL)
			prev_stmt->dn_list = NULL;

		/*
		 * Generate clause for statements preceding the if/while.
		 */
		if (first_stmt_in_basic_block != NULL) {
			dt_sugar_new_basic_block(dp, precondition,
			    first_stmt_in_basic_block);
		}

		if (stmt->dn_kind == DT_NODE_IF)
			dt_sugar_do_if(dp, stmt, precondition);
		else
			dt_sugar_do_while(dp, stmt, precondition);

		first_stmt_in_basic_block = NULL;

		prev_stmt = stmt;
	}

	/* generate clause for statements after last if/while. */
	if (first_stmt_in_basic_block != NULL) {
		dt_sugar_new_basic_block(dp, precondition,
		    first_stmt_in_basic_block);
	}
}

/*
 * Generate a new clause which will set the error variable when an error occurs.
 * Only one of these clauses is created per program (e.g. script file).
 * The clause is:
 *
 * dtrace:::ERROR{ self->%error = 1; }
 */
static dt_node_t *
dt_sugar_makeerrorclause(void)
{
	dt_node_t *acts, *pdesc;

	pdesc = dt_node_pdesc_by_name(strdup("dtrace:::ERROR"));

	acts = dt_node_statement(dt_node_op2(DT_TOK_ASGN,
	    dt_sugar_new_error_var(), dt_node_int(1)));

	return (dt_node_clause(pdesc, NULL, acts));
}

/*
 * Transform the super-clause into straight-D, returning the new list of
 * sub-clauses.
 */
dt_node_t *
dt_compile_sugar(dtrace_hdl_t *dtp, dt_node_t *clause)
{
	dt_node_t *pdesc;
	dt_sugar_parse_t dp = { 0 };
	int condid = 0;

	dp.dtsp_dtp = dtp;
	dp.dtsp_pdescs = clause->dn_pdescs;

	/* make dt_node_int() generate an "int"-typed integer */
	yyintdecimal = B_TRUE;
	yyintsuffix[0] = '\0';
	yyintprefix = 0;

	/*
	 * Determine if all of the probes in the description are :return
	 * probes.  If so, they are allowed to use entry_* variables.
	 */
	dp.dtsp_in_return = B_TRUE;
	for (pdesc = dp.dtsp_pdescs; pdesc != NULL; pdesc = pdesc->dn_list) {
		if (strcmp(pdesc->dn_desc->dtpd_name, "return") != 0)
			dp.dtsp_in_return = B_FALSE;
	}

	dt_sugar_visit_all(&dp, clause);

	if (dp.dtsp_need_entry) {
		/*
		 * If there is an entry_* variable, we must predicate this
		 * return clause so that it is only executed if the
		 * corresponding entry clause executed.  This must be
		 * condition ID #1; the number is hardcoded below in
		 * new_condition1_clause().
		 */
		dp.dtsp_num_conditions++;
		condid = dp.dtsp_num_conditions;
		assert(condid == 1);
	}

	if (dp.dtsp_num_ifs == 0 && dp.dtsp_num_whiles == 0 &&
	    dp.dtsp_num_conditions == 0) {
		/*
		 * There is nothing that modifies the number of clauses.
		 * Use the existing clause as-is, with its predicate intact.
		 * This ensures that in the absence of D++, the body of the
		 * clause can create a variable that is referenced in the
		 * predicate.
		 */
		dt_sugar_append_clause(&dp, dt_node_clause(clause->dn_pdescs,
		    clause->dn_pred, clause->dn_acts));
	} else {
		if (clause->dn_pred != NULL) {
			condid = dt_sugar_new_condition(&dp,
			    clause->dn_pred, condid);
		}

		if (clause->dn_acts == NULL) {
			/*
			 * dt_sugar_visit_stmts() does not emit a clause with
			 * an empty body (e.g. if there's an empty "if" body),
			 * but we need the empty body here so that we
			 * continue to get the default tracing action.
			 */
			dt_sugar_new_basic_block(&dp, condid, NULL);
		} else {
			dt_sugar_visit_stmts(&dp, clause->dn_acts, condid);
		}
	}

	dt_sugar_append_clause(&dp, dp.dtsp_append_clauses);
	if (dp.dtsp_need_entry) {
		dt_sugar_prepend_clause(&dp,
		    dt_sugar_new_condition1_clause(&dp));
	}
	if (dp.dtsp_num_conditions != 0) {
		dt_sugar_prepend_clause(&dp,
		    dt_sugar_new_clearerror_clause(&dp));
	}
	if (dp.dtsp_need_entry) {
		dt_sugar_prepend_clause(&dp, dt_sugar_new_entry_clause(&dp));
		dt_sugar_append_clause(&dp, dt_sugar_new_return_clause(&dp));
	}
	if (dp.dtsp_clause_list != NULL &&
	    dp.dtsp_clause_list->dn_list != NULL && !dtp->dt_has_sugar) {
		dtp->dt_has_sugar = B_TRUE;
		dt_sugar_prepend_clause(&dp, dt_sugar_makeerrorclause());
	}
	return (dp.dtsp_clause_list);
}
