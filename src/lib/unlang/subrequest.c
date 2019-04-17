/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file unlang/subrequest.c
 * @brief Unlang "subrequest" and "detach" keyword evaluation.
 *
 * @copyright 2006-2019 The FreeRADIUS server project
 */
RCSID("$Id$")

#include "unlang_priv.h"
#include "subrequest_priv.h"

static fr_dict_t *dict_freeradius;

extern fr_dict_autoload_t subrequest_dict[];
fr_dict_autoload_t subrequest_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_request_lifetime;

extern fr_dict_attr_autoload_t subrequest_dict_attr[];
fr_dict_attr_autoload_t subrequest_dict_attr[] = {
	{ .out = &attr_request_lifetime, .name = "Request-Lifetime", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ NULL }
};

static void unlang_max_request_time(UNUSED fr_event_list_t *el, UNUSED struct timeval *now, void *uctx)
{
	REQUEST *request = talloc_get_type_abort(uctx, REQUEST);

	RDEBUG("Reached Request-Lifetime.  Forcibly stopping request");

	if (request->runnable_id >= 0) {
		rad_assert(request->backlog != NULL);
		(void) fr_heap_extract(request->backlog, request);
	}

	talloc_free(request);
}

/** Send a signal from parent request to subrequest
 *
 */
static void unlang_subrequest_signal(UNUSED REQUEST *request, void *ctx, fr_state_signal_t action)
{
	REQUEST			*child = talloc_get_type_abort(ctx, REQUEST);

	unlang_interpret_signal(child, action);
}


/** Resume a subrequest
 *
 */
static unlang_action_t unlang_subrequest_resume(REQUEST *request, rlm_rcode_t *presult, void *rctx)
{
	REQUEST				*child = talloc_get_type_abort(rctx, REQUEST);
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_subrequest_t	*state = talloc_get_type_abort(frame->state, unlang_frame_state_subrequest_t);
	rlm_rcode_t			rcode;
#ifndef NDEBUG
	unlang_resume_t			*mr;
#endif

	/*
	 *	Continue running the child.
	 */
	rcode = unlang_interpret_run(child);
	if (rcode != RLM_MODULE_YIELD) {
		rad_assert(frame->instruction->type == UNLANG_TYPE_RESUME);

		*presult = rcode;

		frame->instruction->type = UNLANG_TYPE_SUBREQUEST; /* for debug purposes */

		if (!state->persist) unlang_subrequest_free(&child);

		/*
		 *	Pass the result back to the module
		 *	that created the subrequest, or
		 *	use it to modify the current section
		 *	rcode.
		 */
		if (state->presult) {
			*state->presult = rcode;
		} else {
			*presult = rcode;
		}

		return UNLANG_ACTION_CALCULATE_RESULT;
	}

#ifndef NDEBUG
	rad_assert(frame->instruction->type == UNLANG_TYPE_RESUME);

	mr = unlang_generic_to_resume(frame->instruction);
	(void) talloc_get_type_abort(mr, unlang_resume_t);

	rad_assert(mr->resume == NULL);
#endif

	/*
	 *	If the child yields, our current frame is still an
	 *	unlang_resume_t.
	 */

	return UNLANG_ACTION_YIELD;
}

static unlang_action_t unlang_subrequest(REQUEST *request,
					 rlm_rcode_t *presult, int *ppriority)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_t			*instruction = frame->instruction;
	unlang_frame_state_subrequest_t	*state = NULL;
	unlang_group_t			*g;
	REQUEST				*child;
	rlm_rcode_t			rcode;
	int				priority;
	unlang_resume_t			*mr;

	if (frame->state) state = talloc_get_type_abort(frame->state, unlang_frame_state_subrequest_t);

	g = unlang_generic_to_group(instruction);
	rad_assert(g->children != NULL);

	/*
	 *	When the subrequest is executed as part of an
	 *	unlang section (i.e. the "subrequest" keyword was used)
	 *	we won't have a frame->state.
	 *	In this case we crib most of the necessary
	 *	configuration for the subrequest from the parent.
	 *
	 *	If this function is being called because
	 *	unlang_interpret_push_subrequest was called, the frame->state
	 *	should be filled out by unlang_interpret_push_subrequest.
	 */
	if (!state) {
		frame->state = state = talloc(stack, unlang_frame_state_subrequest_t);
		state->child = unlang_io_child_alloc(request, g->children,
						     request->server_cs, request->dict,
						     frame->result,
						     UNLANG_NEXT_SIBLING, UNLANG_DETACHABLE);
		if (!state->child) {
			rcode = RLM_MODULE_FAIL;
			priority = instruction->actions[*presult];

		calculate_result:
			/*
			 *	Pass the result back to the module
			 *	that created the subrequest, or
			 *	use it to modify the current section
			 *	rcode.
			 */
			if (state->presult) {
				*state->presult = rcode;
				return UNLANG_ACTION_CALCULATE_RESULT;
			}

			*presult = rcode;
			*ppriority = priority;

			return UNLANG_ACTION_CALCULATE_RESULT;
		}

		/*
		 *	Probably not a great idea to set this
		 *	to presult, as it could be a pointer
		 *	to an rlm_rcode_t somewhere on the stack
		 *      which could be invalidated between
		 *	unlang_subrequest being called
		 *	and unlang_subrequest_resume being called.
		 *
		 *	...so we just set it to NULL and interpret
		 *	that as use the presult that was passed
		 *	in to the currently executing function.
		 */
		state->presult = NULL;
		state->persist = false;
	}
	child = state->child;

	RDEBUG2("- creating subrequest (%s)", child->name);

	/*
	 *	Run the child in the same section as the master.  If
	 *	we want to run a different virtual server, we have to
	 *	create a "server" keyword.
	 *
	 *	The only difficult there is setting child->async
	 *	to... some magic value. :( That code should be in a
	 *	virtual server callback, and not directly in the
	 *	interpreter.
	 */
	rcode = unlang_interpret_run(child);
	if (rcode != RLM_MODULE_YIELD) {
		if (!state->persist) unlang_subrequest_free(&child);

		priority = instruction->actions[*presult];

		goto calculate_result;
	}

	/*
	 *	As a special case, if the child instruction is
	 *	"detach", detach the child, insert the child into the
	 *	runnable queue, and keep going with the parent.
	 *
	 *	The unlang_detach() interpreter function takes care of
	 *	calling request_detach() for the child.
	 */
	{
		unlang_stack_t		*child_stack = child->stack;
		unlang_stack_frame_t	*child_frame = &child_stack->frame[child_stack->depth];
		unlang_t		*child_instruction = child_frame->instruction;

		if (child_instruction->type == UNLANG_TYPE_DETACH) {
			rad_assert(child->backlog != NULL);

			if (fr_heap_insert(child->backlog, child) < 0) {
				RPERROR("Failed inserting child into backlog");
				return UNLANG_ACTION_STOP_PROCESSING;
			}

			RDEBUG2("- detaching child request (%s)", child->name);

			/*
			 *	Tell the interpreter to skip the "detach"
			 *	stack frame when it continues.
			 */
			child_frame->instruction = child_frame->next;
			if (child_frame->instruction) child_frame->next = child_frame->instruction->next;

			rcode = RLM_MODULE_NOOP;
			priority = 0;

			goto calculate_result;
		} /* else the child yielded, so we have to yield */
	}

	/*
	 *	Create the "resume" stack frame, and have it replace our stack frame.
	 */
	mr = unlang_interpret_resume_alloc(request, NULL, NULL, child);
	if (!mr) {
		rcode = RLM_MODULE_FAIL;
		priority = instruction->actions[*presult];
		goto calculate_result;
	}

	*presult = RLM_MODULE_YIELD;
	return UNLANG_ACTION_YIELD;
}

static unlang_action_t unlang_detach(REQUEST *request,
				     rlm_rcode_t *presult, int *priority)
{
	VALUE_PAIR		*vp;
	unlang_stack_t		*stack = request->stack;
	unlang_stack_frame_t	*frame = &stack->frame[stack->depth];
	unlang_t		*instruction = frame->instruction;

	rad_assert(instruction->parent->type == UNLANG_TYPE_SUBREQUEST);
	RDEBUG2("%s", unlang_ops[instruction->type].name);

	rad_assert(request->parent != NULL);

	if (request_detach(request) < 0) {
		ERROR("Failed detaching child");
		*presult = RLM_MODULE_FAIL;
		*priority = 0;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	/*
	 *	Set Request Lifetime
	 */
	vp = fr_pair_find_by_da(request->control, attr_request_lifetime, TAG_ANY);
	if (!vp || (vp->vp_uint32 > 0)) {
		struct timeval when;
		const fr_event_timer_t **ev_p;

		gettimeofday(&when, NULL);

		if (!vp) {
			when.tv_sec += 30; /* default to 30s if not set */

		} else if (vp->vp_uint32 > 3600) {
			RWARN("Request-Timeout can be no more than 3600");
			when.tv_sec += 3600;

		} else if (vp->vp_uint32 < 5) {
			RWARN("Request-Timeout can be no less than 5");
			when.tv_sec += 5;

		} else {
			when.tv_sec += vp->vp_uint32;
		}

		ev_p = talloc_size(request, sizeof(*ev_p));
		memset(ev_p, 0, sizeof(*ev_p));

		(void) fr_event_timer_insert(request, request->el, ev_p,
					     &when, unlang_max_request_time, request);
	}

	/*
	 *	request_detach() sets the backlog
	 *	it does set the backlog...
	 */
	rad_assert(request->backlog != NULL);

	*presult = RLM_MODULE_YIELD;
	return UNLANG_ACTION_YIELD;
}

/** Free a child request, detaching it from its parent and freeing allocated memory
 *
 * @param[in] child to free.
 */
void unlang_subrequest_free(REQUEST **child)
{
	request_detach(*child);	/* Doesn't actually detach the client, just does cleanups */
	talloc_free(*child);
	*child = NULL;
}

static unlang_t subrequest_instruction = {
	.type = UNLANG_TYPE_SUBREQUEST,
	.name = "subrequest",
	.debug_name = "subrequest",
	.actions = {
		[RLM_MODULE_REJECT]	= 0,
		[RLM_MODULE_FAIL]	= MOD_ACTION_RETURN,	/* Exit out of nested levels */
		[RLM_MODULE_OK]		= 0,
		[RLM_MODULE_HANDLED]	= 0,
		[RLM_MODULE_INVALID]	= 0,
		[RLM_MODULE_USERLOCK]	= 0,
		[RLM_MODULE_NOTFOUND]	= 0,
		[RLM_MODULE_NOOP]	= 0,
		[RLM_MODULE_UPDATED]	= 0
	},
};

/** Push a pre-existing child back onto the stack as a subrequest
 *
 * The child *MUST* have been allocated with unlang_subrequest_push. Instruction *MUST* belong to
 * the same virtual server as the original request was executed in.
 *
 * After the child is no longer required it *MUST* be freed with #unlang_subrequest_free.
 * It's not enough to free it with talloc_free.
 *
 * @param[in] out		Where to write the result of the subrequest.
 * @param[in] child		to push.
 * @param[in] parent		of the child.
 * @param[in] instruction	to execute in the child.
 * @param[in] default_rcode	for instruction being executed.
 * @param[in] top_frame		Set to UNLANG_TOP_FRAME if the interpreter should return.
 *				Set to UNLANG_SUB_FRAME if the interprer should continue.
 */
void unlang_subrequest_push_again(rlm_rcode_t *out, REQUEST *child, REQUEST *parent,
				  unlang_t *instruction, rlm_rcode_t default_rcode, bool top_frame)
{
	unlang_frame_state_subrequest_t	*state;
	unlang_stack_t			*stack = parent->stack;
	unlang_stack_frame_t		*frame;

	rad_assert(child->parent == parent);

	/*
	 *	Push a new subrequest frame onto the stack
	 */
	unlang_interpret_push(stack, &subrequest_instruction, RLM_MODULE_UNKNOWN, UNLANG_NEXT_STOP, top_frame);
	frame = &stack->frame[stack->depth];

	/*
	 *	Allocate a state for the subrequest
	 *	This lets us override the normal request
	 *      the subrequest instruction would alloc.
	 */
	MEM(frame->state = state = talloc_zero(stack, unlang_frame_state_subrequest_t));
	state->presult = out;
	state->persist = true;
	state->child = child;

	/*
	 *	Push the instruction to execute onto the child's stack.
	 */
	stack = child->stack;
	unlang_interpret_push(stack, instruction, default_rcode, UNLANG_NEXT_SIBLING, UNLANG_SUB_FRAME);
	stack->frame[stack->depth].top_frame = true;
}

/** Allocate a child and set it up for execution
 *
 * After the child is no longer required it *MUST* be freed with #unlang_subrequest_free.
 * It's not enough to free it with talloc_free.  This will be done automatically if
 * no child pointer is provided to store a pointer to the child.
 *
 * @param[in] out		Where to write the result of the subrequest.
 * @param[out] child		The child we allocated.  The caller can then add
 *				VALUE_PAIRs and request data before the child is executed.
 * @param[in] parent		to hang child request off of.
 * @param[in] server_cs		Server to execute subrequest in.
 * @param[in] instruction	Where to start the child.
 * @param[in] namespace		to use for the subrequest.
 * @param[in] default_rcode	To use when the child enters the section.
 * @param[in] top_frame		Set to UNLANG_TOP_FRAME if the interpreter should return.
 *				Set to UNLANG_SUB_FRAME if the interprer should continue.
 */
void unlang_subrequest_push(rlm_rcode_t *out, REQUEST **child,
			    REQUEST *parent,
			    CONF_SECTION *server_cs, unlang_t *instruction, fr_dict_t const *namespace,
			    rlm_rcode_t default_rcode,
			    bool top_frame)
{

	unlang_frame_state_subrequest_t	*state;
	unlang_stack_t			*stack = parent->stack;
	unlang_stack_frame_t		*frame;

	/*
	 *	Push a new subrequest frame onto the stack
	 */
	unlang_interpret_push(stack, &subrequest_instruction, RLM_MODULE_UNKNOWN, UNLANG_NEXT_STOP, top_frame);
	frame = &stack->frame[stack->depth];

	/*
	 *	Allocate a state for the subrequest
	 *	This lets us override the normal request
	 *      the subrequest instruction would alloc.
	 */
	MEM(frame->state = state = talloc_zero(stack, unlang_frame_state_subrequest_t));
	state->presult = out;
	state->persist = child ? true : false;

	/*
	 *	Pushes instruction onto child
	 */
	state->child = unlang_io_child_alloc(parent, instruction,
					     server_cs, namespace,
					     default_rcode,
					     UNLANG_NEXT_SIBLING, UNLANG_NORMAL_CHILD);
}

int unlang_subrequest_op_init(void)
{
	if (fr_dict_autoload(subrequest_dict) < 0) {
		PERROR("%s", __FUNCTION__);
		return -1;
	}
	if (fr_dict_attr_autoload(subrequest_dict_attr) < 0) {
		PERROR("%s", __FUNCTION__);
		return -1;
	}

	unlang_register(UNLANG_TYPE_SUBREQUEST,
			   &(unlang_op_t){
				.name = "subrequest",
				.func = unlang_subrequest,
				.signal = unlang_subrequest_signal,
				.resume = unlang_subrequest_resume,
				.debug_braces = true
			   });

	unlang_register(UNLANG_TYPE_DETACH,
			   &(unlang_op_t){
				.name = "detach",
				.func = unlang_detach,
			   });

	return 0;
}

void unlang_subrequest_op_free(void)
{
	fr_dict_autofree(subrequest_dict);
}
