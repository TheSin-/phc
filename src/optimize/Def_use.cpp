
#include "boost/tuple/tuple.hpp"

#include "process_ir/debug.h"

#include "Def_use.h"
#include "Var_set.h"
#include "ssa/Virtual_variable.h"


using namespace MIR;
using namespace std;
using namespace boost;

/*
 * SSA-web. 
 *
 * For now, use the SSA-web (which doesnt actually rely on SSA form, nor is
 * it built during it) both for getting into SSA form, and for creating the
 * SSA web.
 */

SSA_edge::SSA_edge (VARIABLE_NAME* var, SSA_op* op)
: variable_name (var)
, op (op)
{
}

void
SSA_edge::dump ()
{
	// TODO: mention if its a use or a def?
	DEBUG ("Edge from " << *variable_name->get_ssa_var_name () << ": ");
	op->dump ();
}


Def_use_web::Def_use_web (Var_set* aliases)
: aliases (aliases)
{
}

SSA_op_list*
Def_use_web::get_defs (VARIABLE_NAME* use, int flags)
{
	SSA_op_list* result = new SSA_op_list;

	foreach (SSA_edge* edge, use_def_chains [use])
		result->push_back (edge->op);

	return result;
}

SSA_op_list*
Def_use_web::get_uses (VARIABLE_NAME* def, int flags)
{
	// Its possible to have defs without uses.
	if (!def_use_chains.has (def))
		return new SSA_op_list ();

	SSA_op_list* result = new SSA_op_list;
	foreach (SSA_edge* edge, def_use_chains[def])
	{
		if (edge->op->type_flag & flags)
			result->push_back (edge->op);
	}
	return result;
}


SSA_op*
Def_use_web::get_def (VARIABLE_NAME* use)
{
	assert (has_def (use));
	SSA_op_list* defs = get_defs (use, SSA_STMT);

	assert (defs->size() == 1);
	return defs->front ();
}

bool
Def_use_web::has_def (VARIABLE_NAME* use)
{
	return use_def_chains[use].size () >= 1;
}



VARIABLE_NAME_list*
Def_use_web::get_block_defs (Basic_block* bb, int flags)
{
	// There wont be any duplicates here.
	VARIABLE_NAME_list* result = new VARIABLE_NAME_list;

	// Go through the use-def result, finding those who's BB == BB
	VARIABLE_NAME* key;
	SSA_edge_list edge_list;
	foreach (tie (key, edge_list), use_def_chains)
	{
		foreach (SSA_edge* edge, edge_list)
		{
			if ((edge->op->type_flag & flags) // filter based on flags
				&& edge->op->get_bb () == bb)
			{
				// Dont insert the key itself, it may be a different var_name
				// with the same value, but we return exact var_name.
				result->push_back (edge->variable_name);
			}
		}
	}
	return result;

}

VARIABLE_NAME_list*
Def_use_web::get_block_uses (Basic_block* bb, int flags)
{
	// Dont use a set here. There may be multiple uses of the same variable.
	VARIABLE_NAME_list* result = new VARIABLE_NAME_list;

	// Go through the use-def result, finding those who's BB == BB
	VARIABLE_NAME* key;
	SSA_edge_list edge_list;
	foreach (tie (key, edge_list), def_use_chains)
	{
		foreach (SSA_edge* edge, edge_list)
		{
			if ((edge->op->type_flag & flags) // filter based on flags
				&& edge->op->get_bb () == bb)
			{
				// Dont insert the key itself, it may be the wrong var_name.
				result->push_back (edge->variable_name);
			}
		}
	}
	return result;
}


/*
 * Calculate the def-use web
 */
void
Def_use_web::add_use (MIR::Rvalue* def, SSA_op* use, bool add_mu)
{
	if (isa<VARIABLE_NAME> (def))
		add_use (dyc<VARIABLE_NAME> (def), use, add_mu);
}


void
Def_use_web::add_use (MIR::VARIABLE_NAME* def, SSA_op* use, bool add_mu)
{
	SSA_edge* edge = new SSA_edge (def, use);
	edge->variable_name = def;
	def_use_chains[def].push_back (edge);

	DEBUG ("Adding a def_use edge from ");
	debug (def);
	DEBUG ("to ");
	use->dump ();
	DEBUG (endl);

	if (add_mu)
		add_mus (use->get_bb(), def);
}

void
Def_use_web::add_def (MIR::VARIABLE_NAME* use, SSA_op* def, bool add_chi)
{
	SSA_edge* edge = new SSA_edge (use, def);

	// When used on pre-SSA form, there can be many defs.
	use_def_chains[use].push_back (edge);

	DEBUG ("Adding a use_def edge from ");
	debug (use);
	DEBUG ("to ");
	def->dump ();
	DEBUG (endl);

	if (add_chi)
		add_chis (def->get_bb (), use);
}


void
Def_use_web::add_may_def (MIR::VARIABLE_NAME* var, SSA_op* def)
{
	// TODO: Whether we are in SSA form is, unfortunately, is only signalled by
	// the alias set. Fix.

	// Only add may_defs if we are not in SSA form.
	if (aliases)
	{
		Basic_block* bb = def->get_bb ();
		bb->add_chi_node (var, var);
		VARIABLE_NAME* clone = var->clone ();
		add_def (var, new SSA_chi (bb, var, clone), false);
		add_use (clone, new SSA_chi (bb, var, clone), false);
	}
}

/*
 * ADD_MUS and ADD_CHIS only work before SSA form. In SSA-form, the aliasing is
 * explicitly represented using the CHI/MUs, so adding them is incorrect.
 */
void
Def_use_web::add_mus (Basic_block* bb, VARIABLE_NAME* use)
{
	if (aliases && aliases->has (use))
	{
		assert (use->in_ssa == false);
		foreach (VARIABLE_NAME* alias, *aliases)
			if (!alias->equals (use))
			{
				bb->add_mu_node (alias);
				add_use (alias, new SSA_mu (bb, alias), false);
			}
	}
}

void
Def_use_web::add_chis (Basic_block* bb, VARIABLE_NAME* def)
{
	// TODO: CHIs are not created for the parameters to a method call. They
	// clearly should be.

	// TODO:
	// There is a special problem that occurs when you have two indirect
	// assignments in the same statement. Suppose you have:
	//
	//		$x = my_func ($y);
	//
	//	Then you have to add CHIs for the aliases of $y AND for the aliases of
	//	$x. However, the CHIs for the variables which alias $x do not occur at
	//	the same time as the variables which alias $y. Instead, $y's aliases
	//	should occur after the method invocation, and $x's aliases should occur
	//	after the assignment to $x. $x is in the first set, but not in the
	//	second. Anything which aliases both $y and $x gets 2 CHIs. $y will get
	//	2 CHIs.
	//
	//	So:
	//		$x0 = 5;
	//		$x2 = my_func ($y0);
	//		$x1 = CHI ($x0); // METHOD
	//		$y1 = CHI ($y0); // METHOD
	//		$y2 = CHI ($y1); // ASSIGNMENT
	//
	//	Some interesting notes:
	//		- There is no use of $x1.
	//		- This problem only occurs when there can be > 1 assignment, which
	//		can only occur in assignments from method invocations.
	//		- The same variable can get given a CHI from each of a method's
	//		parameters, but that is only 1 CHI overall.
	//		- There can only be 1 or 2 CHIs, not more (see above).
	//		- If we were to use zero versions, the first chi would have a zero
	//		version, since it has no real use.
	//		- This is only really important for getting the numbering right.
	//		Anywhere that uses phis would get them all.
	//
	//
	//
	// Some possible solutions:
	//		Associate CHIs with other definitions, maybe each def has a list of
	//		maydefs. But there is no def in the function call to associate it
	//		with, though I think one could safely be added (the MU would ensure
	//		that it is not overwritten).
	//
	//		Allow multiple CHIs in the CHI list. If there is more than 1 of the
	//		same name, they are assumed to be in the same order in which they
	//		occur. I believe that the only variable which will only occur once
	//		in the variable on the RHS.
	//		 - but what about when we have small alias sets? Then we dont really
	//		 know which is which.
	//		 - besides, keeping track of that seems very buggy
	//
	// XXX: I like this one best, I think.
	//		Have two CHI lists, one for 'mid' statement, one for
	//		'post-statement'. Aliases of assignments go in the latter. Aliases
	//		due to the call go in the former.
	//
	//	I think this will also solve the problem of 'vanishing globals'. In the case of:
	//		global $x;
	//		$x = 5;
	//
	//	The global statement is remove because $x_0 is not used. However, $x = 5
	//	is a must_def of $x_0. We can model it as a may-def however, giving it a
	//	CHI in the mid-part, and the CHI in the post-statement part gives it the
	//	new value.
	//
	//	The global statement will not in fact be solved by this. If there were
	//	an extra statement, $x = 8 after $x = 5, then $x = 5 would be kept live
	//	by this 'Early CHI'.
	//
	//	How about each assignment to an alias gets a MU of the variables in the
	//	statement which created the alias.
	if (aliases && aliases->has (def))
	{
		assert (def->in_ssa == false);

		foreach (VARIABLE_NAME* alias, *aliases)
			if (!alias->equals (def))
			{
				bb->add_chi_node (alias, alias);
				VARIABLE_NAME* clone = alias->clone ();
				add_def (alias, new SSA_chi (bb, alias, clone), false);
				add_use (clone, new SSA_chi (bb, alias, clone), false);
			}
	}
}

void
Def_use_web::add_call_clobbering (Basic_block* bb)
{
	// All global variables, and any other variables which escape the function,
	// can be defined (aka call-clobbered), or used, by a function call or
	// function exit point.

	if (aliases)
	{
		foreach (VARIABLE_NAME* alias, *aliases)
		{
			bb->add_chi_node (alias, alias);
			VARIABLE_NAME* clone = alias->clone ();
			add_def (alias, new SSA_chi (bb, alias, clone), false);
			add_use (clone, new SSA_chi (bb, alias, clone), false);
		}
	}
}



void
Def_use_web::visit_entry_block (Entry_block* bb)
{
	add_call_clobbering (bb);

	foreach (Formal_parameter* param, *bb->method->signature->formal_parameters)
		add_def (param->var->variable_name, new SSA_formal (bb));
}

void
Def_use_web::visit_exit_block (Exit_block* bb)
{
	add_call_clobbering (bb);
}

void
Def_use_web::visit_branch_block (Branch_block* bb)
{
	add_use (bb->branch->variable_name, new SSA_branch (bb));
}

void
Def_use_web::visit_phi_node (Basic_block* bb, VARIABLE_NAME* phi_lhs)
{
	foreach (VARIABLE_NAME* use, *bb->get_phi_args (phi_lhs))
		add_use (use, new SSA_phi (bb, phi_lhs));

	add_def (phi_lhs, new SSA_phi (bb, phi_lhs));
}

void
Def_use_web::visit_chi_node (Basic_block* bb, VARIABLE_NAME* lhs, VARIABLE_NAME* rhs)
{
	assert (aliases == NULL);
	add_def (lhs, new SSA_chi (bb, lhs, rhs));
	add_use (rhs, new SSA_chi (bb, lhs, rhs));
}

void
Def_use_web::visit_mu_node (Basic_block* bb, VARIABLE_NAME* rhs)
{
	assert (aliases == NULL);
	add_use (rhs, new SSA_mu (bb, rhs));
}

void
Def_use_web::visit_assign_array (Statement_block* bb, MIR::Assign_array* in)
{
	add_use (in->lhs, new SSA_stmt (bb));
	add_use (in->rhs, new SSA_stmt (bb));
	add_use (in->index, new SSA_stmt (bb));
	add_def (get_virtual (bb, in), new SSA_stmt (bb));

	// $x[0] = 5;
	// may convert $x from a scalar to an array.
	add_may_def (in->lhs, new SSA_stmt (bb));
}

void
Def_use_web::visit_assign_field (Statement_block*, MIR::Assign_field * in)
{
	PUNT;
}

void
Def_use_web::visit_assign_var (Statement_block* bb, MIR::Assign_var* in)
{
	add_def (in->lhs, new SSA_stmt (bb), !in->is_ref);
	visit_expr (bb, in->rhs);
}

void
Def_use_web::visit_assign_var_var (Statement_block*, MIR::Assign_var_var* in)
{
	PUNT;
}

void
Def_use_web::visit_eval_expr (Statement_block* bb, MIR::Eval_expr* in)
{
	visit_expr (bb, in->expr);
}

void
Def_use_web::visit_foreach_end (Statement_block* bb, MIR::Foreach_end* in)
{
	add_use (in->array, new SSA_stmt (bb));
	add_def (get_virtual (bb, in->iter), new SSA_stmt (bb));
}

void
Def_use_web::visit_foreach_next (Statement_block* bb, MIR::Foreach_next* in)
{
	add_use (in->array, new SSA_stmt (bb));
	add_def (get_virtual (bb, in->iter), new SSA_stmt (bb));
}

void
Def_use_web::visit_foreach_reset (Statement_block* bb, MIR::Foreach_reset* in)
{
	add_use (in->array, new SSA_stmt (bb));
	add_def (get_virtual (bb, in->iter), new SSA_stmt (bb));
}

void
Def_use_web::visit_global (Statement_block* bb, MIR::Global* in)
{
	// For SSA creation, this is a def.
	if (isa<VARIABLE_NAME> (in->variable_name))
		add_def (dyc<VARIABLE_NAME> (in->variable_name), new SSA_stmt (bb), false);
	else
		PUNT; // global var-var!
}

void
Def_use_web::visit_pre_op (Statement_block*, MIR::Pre_op* in)
{
	PUNT;
}

void
Def_use_web::visit_assign_next (Statement_block* bb, MIR::Assign_next* in)
{
	add_use (in->lhs, new SSA_stmt (bb));
	add_use (in->rhs, new SSA_stmt (bb));

	add_def (get_virtual (bb, in), new SSA_stmt (bb));
}

void
Def_use_web::visit_return (Statement_block* bb, MIR::Return* in)
{
	// We put the MUs in the exit block, as not all paths have a return block.
	add_use (in->rvalue, new SSA_stmt (bb));
}

void
Def_use_web::visit_ssa_pre_op (Statement_block* bb, MIR::SSA_pre_op* in)
{
	add_use (in->use, new SSA_stmt (bb));
	add_def (in->def, new SSA_stmt (bb));
}

void
Def_use_web::visit_static_declaration (Statement_block*, MIR::Static_declaration* in)
{
	PUNT;
}

void
Def_use_web::visit_throw (Statement_block*, MIR::Throw* in)
{
	PUNT;
}

void
Def_use_web::visit_try (Statement_block*, MIR::Try* in)
{
	PUNT;
}

void
Def_use_web::visit_unset (Statement_block* bb, MIR::Unset* in)
{
	assert (in->target == NULL);
	assert (isa<VARIABLE_NAME> (in->variable_name));

	// TODO: this defines a virtual-variable
	if (in->array_indices->size ())
	{
		add_use (dyc<VARIABLE_NAME> (in->variable_name), new SSA_stmt (bb));
		foreach (Rvalue* rv, *in->array_indices)
		{
			add_use (rv, new SSA_stmt (bb));
		}
	}
	else
		add_def (dyc<VARIABLE_NAME> (in->variable_name), new SSA_stmt (bb));
}

/*
 * Exprs
 */

void
Def_use_web::visit_array_access (Statement_block* bb, Array_access* in)
{
	add_use (in->variable_name, new SSA_stmt (bb));
	add_use (in->index, new SSA_stmt (bb));
	add_use (get_virtual (bb, in), new SSA_stmt (bb));

	// In the case of
	//		$x =& $y[$i]
	// $y can be defined by this.
	// TODO: So can virt($y[$i])
	// TODO: restrict to only if is_ref is set
	add_may_def (in->variable_name, new SSA_stmt (bb));
}

void
Def_use_web::visit_bin_op (Statement_block* bb, Bin_op* in)
{
	add_use (in->left, new SSA_stmt (bb));
	add_use (in->right, new SSA_stmt (bb));
}

void
Def_use_web::visit_cast (Statement_block* bb, Cast* in)
{
	add_use (in->variable_name, new SSA_stmt (bb));
}

void
Def_use_web::visit_constant (Statement_block* bb, Constant* in)
{
}

void
Def_use_web::visit_field_access (Statement_block* bb, Field_access* in)
{
	PUNT;
}

void
Def_use_web::visit_foreach_get_key (Statement_block* bb, Foreach_get_key* in)
{
	add_use (in->array, new SSA_stmt (bb));
	add_use (get_virtual (bb, in->iter), new SSA_stmt (bb));
}

void
Def_use_web::visit_foreach_get_val (Statement_block* bb, Foreach_get_val* in)
{
	add_use (in->array, new SSA_stmt (bb));
	add_use (get_virtual (bb, in->iter), new SSA_stmt (bb));
}

void
Def_use_web::visit_foreach_has_key (Statement_block* bb, Foreach_has_key* in)
{
	add_use (in->array, new SSA_stmt (bb));
	add_use (get_virtual (bb, in->iter), new SSA_stmt (bb));
}

void
Def_use_web::visit_instanceof (Statement_block* bb, Instanceof* in)
{
	PUNT;
}

void
Def_use_web::visit_isset (Statement_block* bb, Isset* in)
{
	PUNT;
}

void
Def_use_web::visit_method_invocation (Statement_block* bb, Method_invocation* in)
{
	add_call_clobbering (bb);

	if (isa<Variable_method> (in->method_name))
		PUNT;

	foreach (Actual_parameter* param, *in->actual_parameters)
	{
		if (VARIABLE_NAME* var = dynamic_cast<VARIABLE_NAME*> (param->rvalue))
		{
			add_use (var, new SSA_stmt (bb));

			// Call-time pass-by-ref
			if (param->is_ref)
				add_may_def (var, new SSA_chi (bb, var, var->clone ()));
		}
	}
}

void
Def_use_web::visit_new (Statement_block* bb, New* in)
{
	add_call_clobbering (bb);
	PUNT;
}

void
Def_use_web::visit_unary_op (Statement_block* bb, Unary_op* in)
{
	add_use (in->variable_name, new SSA_stmt (bb));
}

void
Def_use_web::visit_variable_name (Statement_block* bb, VARIABLE_NAME* in)
{
	add_use (in, new SSA_stmt (bb));
}

void
Def_use_web::visit_variable_variable (Statement_block* bb, Variable_variable* in)
{
	PUNT;
}


void
Def_use_web::dump ()
{
	CHECK_DEBUG ();

	cdebug << "Use-def web (" << use_def_chains.size() << "):\n";
	pair<VARIABLE_NAME*, SSA_edge_list> p;
	foreach (p, use_def_chains)
	{
		cdebug << "SSA edges for ";
		debug (p.first);

		foreach (SSA_edge* edge, p.second)
		{
			edge->dump ();
		}
		cdebug << endl;
	}

	cdebug << "Def-use web (" << def_use_chains.size() << "):\n";
	foreach (p, def_use_chains)
	{
		cdebug << "SSA edges for ";
		debug (p.first);

		foreach (SSA_edge* edge, p.second)
		{
			edge->dump ();
		}
		cdebug << endl;
	}
}

void
Def_use_web::consistency_check ()
{
	return;

	// Is the first variable we find in SSA.
	bool in_ssa = (*use_def_chains.begin ()).first->in_ssa;
	if (!in_ssa)
		return;


	// Check that each use has 0 or 1 def.
	VARIABLE_NAME* key;
	SSA_edge_list edge_list;
	foreach (tie (key, edge_list), use_def_chains)
	{
		assert (edge_list.size () < 2);
	}


	// Check that each key is in SSA form
	foreach (tie (key, edge_list), use_def_chains)
		assert (key->in_ssa);
	
	foreach (tie (key, edge_list), def_use_chains)
		assert (key->in_ssa);
}
