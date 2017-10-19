/* radare - LGPL - Copyright 2017 - pancake */

/* blaze is the codename for the new analysis engine for radare2 */
/* by --pancake thanks to @defragger and @nguyen for ideas */

#include <r_core.h>

typedef enum bb_type {
	TRAP,
	NORMAL,
	JUMP,
	FAIL,
	CALL,
	END,
} bb_type_t;

typedef struct bb {
	ut64 start;
	ut64 end;
	ut64 jump;
	ut64 fail;
	int score;
	int called;
	int reached;
	bb_type_t type;
} bb_t;

typedef struct fcn {
	ut64 addr;
	ut64 size;
	RList *bbs;
	st64 score;
	ut64 ends;
} fcn_t;

static bool fcnAddBB (fcn_t *fcn, bb_t* block) {
	if (!fcn) {
		eprintf ("No function given to add a basic block\n");
		return false;
	}
	fcn->score += block->score;
	if (block->type == END) {
		fcn->ends++;
	}
	if (!fcn->bbs) {
		eprintf ("Block list not initialized\n");
		return false;
	}
	r_list_append (fcn->bbs, block);
	return true;
}

static fcn_t* fcnNew (bb_t *block) {
	fcn_t* fcn = R_NEW0 (fcn_t);
	if (!fcn) {
		eprintf ("Failed to allocate memory for function\n");
		return NULL;
	}
	fcn->addr = block->start;
	fcn->bbs = r_list_new ();
	if (!fcnAddBB (fcn, block)) {
		eprintf ("Failed to add block to function\n");
	}
	return fcn;
}

static void fcnFree (fcn_t *fcn) {
	r_list_free (fcn->bbs);
	free (fcn);
}

static int bbCMP (void *_a, void *_b) {
	bb_t *a = (bb_t*)_a;
	bb_t *b = (bb_t*)_b;
	return b->start - a->start;
}

static void initBB (bb_t *bb, ut64 start, ut64 end, ut64 jump, ut64 fail, bb_type_t type, int score, int reached, int called) {
	if (bb) {
		bb->start = start;
		bb->end = end;
		bb->jump = jump;
		bb->fail = fail;
		bb->type = type;
		bb->score = score;
		bb->reached = reached;
		bb->called = called;
	}
}

static bool addBB (RList *block_list, ut64 start, ut64 end, ut64 jump, ut64 fail, bb_type_t type, int score) {
	bb_t *bb = (bb_t*) R_NEW0 (bb_t);
	if (!bb) {
		eprintf ("Failed to calloc mem for new basic block!\n");
		return false;
	}
	initBB (bb, start, end, jump, fail, type, score, 0, 0);
	if (jump < UT64_MAX) {
		bb_t *jump_bb = (bb_t*) R_NEW0 (bb_t);
		if (!jump_bb) {
			eprintf ("Failed to allocate memory for jump block\n");
			free (bb);
			return false;
		}
		if (type == CALL) {
			initBB (jump_bb, jump, UT64_MAX, UT64_MAX, UT64_MAX, CALL, 0, 1, 1);
		} else {
			initBB (jump_bb, jump, UT64_MAX, UT64_MAX, UT64_MAX, JUMP, 0, 1, 0);
		}
		r_list_append (block_list, jump_bb);
	}
	if (fail < UT64_MAX) {
		bb_t *fail_bb = (bb_t*) R_NEW0 (bb_t);
		if (!fail_bb) {
			eprintf ("Failed to allocate memory for fail block\n");
			free (bb);
			return false;
		}
		initBB (fail_bb, fail, UT64_MAX, UT64_MAX, UT64_MAX, FAIL, 0, 1, 0);
		r_list_append (block_list, fail_bb);
	}
	r_list_append (block_list, bb);
	return true;
}

void dump_block(bb_t *block) {
	eprintf ("s: 0x%"PFMT64x" e: 0x%"PFMT64x" j: 0x%"PFMT64x" f: 0x%"PFMT64x" t: %d\n"
			, block->start, block->end, block->jump, block->fail, block->type);
}

void dump_blocks (RList* list) {
	RListIter *iter;
	bb_t *block = NULL;
	r_list_foreach (list, iter, block) {
		dump_block(block);
	}
}

static bool checkFunction(fcn_t *fcn) {
	if (fcn && fcn->ends > 0) {
		return true;
	}

	return false;
}

static void printFunctionCommands(RCore *core, fcn_t* fcn, const char *name) {
	if (!fcn) {
		eprintf ("No function given to print\n");
		return;
	}

	RListIter *fcn_iter;
	bb_t *cur = NULL;
	const char *pfx = r_config_get (core->config, "anal.fcnprefix");
	if (!pfx) {
		pfx = "fcn";
	}

	char *_name = name? (char *) name: r_str_newf ("%s.%" PFMT64x, pfx, fcn->addr);
	r_cons_printf ("af+ 0x%08" PFMT64x " %s\n", fcn->addr, _name);
	if (!name) {
		free (_name);
	}

	r_list_foreach (fcn->bbs, fcn_iter, cur) {
		r_cons_printf ("afb+ 0x%08" PFMT64x " 0x%08" PFMT64x " %llu 0x%08"PFMT64x" 0x%08"PFMT64x"\n"
			, fcn->addr, cur->start, cur->end - cur->start, cur->jump, cur->fail);
	}
}

static void createFunction(RCore *core, fcn_t* fcn, const char *name) {
	RAnalFunction *f = NULL;
	if (!fcn) {
		eprintf ("No function given to create\n");
		return;
	}

	RListIter *fcn_iter;
	bb_t *cur = NULL;
	const char *pfx = r_config_get (core->config, "anal.fcnprefix");
	if (!pfx) {
		pfx = "fcn";
	}

	f = r_anal_fcn_new ();
	if (!f) {
		eprintf ("Failed to create new function\n");
		return;
	}

	f->name = name? (char *) name: r_str_newf ("%s.%" PFMT64x, pfx, fcn->addr);
	f->addr = fcn->addr;
	f->bits = core->anal->bits;
	f->cc = r_str_const (r_anal_cc_default (core->anal));
	r_anal_fcn_set_size (f, fcn->size);
	f->type = R_ANAL_FCN_TYPE_FCN;

	r_list_foreach (fcn->bbs, fcn_iter, cur) {
		r_anal_fcn_add_bb (core->anal, f, cur->start, (cur->end - cur->start), cur->jump, cur->fail, 0, NULL);
	}
	if (!r_anal_fcn_insert (core->anal, f)) {
		eprintf ("Failed to insert function\n");
		//TODO free not added function
		return;
	}
}

#define Fhandled(x) sdb_fmt(0, "handled.%"PFMT64x"", x)
R_API bool core_anal_bbs(RCore *core, const char* input) {
	if (!r_io_is_valid_offset (core->io, core->offset, false)) {
		eprintf ("No valid offset given to analyze\n");
		return false;
	}

	Sdb *sdb = NULL;
	ut64 cur = 0;
	ut64 start = core->offset;
	ut64 size = input[0] ? r_num_math (core->num, input + 1) : core->blocksize;
	ut64 b_start = start;
	RAnalOp *op;
	RListIter *iter;
	int block_score = 0;
	RList *block_list;
	bb_t *block = NULL;
	int invalid_instruction_barrier = -20000;


	block_list = r_list_new ();
	if (!block_list) {
		eprintf ("Failed to create block_list\n");
	}

	while (cur < size) {
		if (r_cons_is_breaked ()) {
			break;
		}
		// magic number to fix huge section of invalid code fuzz files
		if (block_score < invalid_instruction_barrier) {
			break;
		}
		op = r_core_anal_op (core, start + cur);

		if (!op || !op->mnemonic) {
			block_score -= 10;
			cur++;
			continue;
		}

		if (op->mnemonic[0] == '?') {
			eprintf ("Cannot analyze opcode at %"PFMT64x"\n", start + cur);
			block_score -= 10;
			cur++;
			continue;
		}
		switch (op->type) {
		case R_ANAL_OP_TYPE_NOP:
			break;
		case R_ANAL_OP_TYPE_CALL:
			if (r_anal_noreturn_at (core->anal, op->jump)) {
				addBB (block_list, b_start, start + cur + op->size, UT64_MAX, UT64_MAX, END, block_score);
				b_start = start + cur + op->size;
				block_score = 0;
			} else {
				addBB (block_list, op->jump, UT64_MAX, UT64_MAX, UT64_MAX, CALL, block_score);
			}
			break;
		case R_ANAL_OP_TYPE_JMP:
			addBB (block_list, b_start, start + cur + op->size, op->jump, UT64_MAX, END, block_score);
			b_start = start + cur + op->size;
			block_score = 0;
			break;
		case R_ANAL_OP_TYPE_TRAP:
			// we dont want to add trap stuff
			if (b_start < start + cur) {
				addBB (block_list, b_start, start + cur, UT64_MAX, UT64_MAX, NORMAL, block_score);
			}
			b_start = start + cur + op->size;
			block_score = 0;
			break;
		case R_ANAL_OP_TYPE_RET:
			addBB (block_list, b_start, start + cur + op->size, UT64_MAX, UT64_MAX, END, block_score);
			b_start = start + cur + op->size;
			block_score = 0;
			break;
		case R_ANAL_OP_TYPE_CJMP:
			addBB (block_list, b_start, start + cur + op->size, op->jump, start + cur + op->size, NORMAL, block_score);
			b_start = start + cur + op->size;
			block_score = 0;
			break;
		case R_ANAL_OP_TYPE_UNK:
		case R_ANAL_OP_TYPE_ILL:
			block_score -= 10;
			break;
		default:
			break;
		}
		cur += op->size;
		r_anal_op_free (op);
		op = NULL;
	}

	RList *result = r_list_newf (free);
	if (!result) {
		r_list_free (block_list);
		eprintf ("Failed to create resulting list\n");
		return false;
	}

	sdb = sdb_new0 ();
	if (!sdb) {
		eprintf ("Failed to initialize sdb db\n");
		r_list_free (block_list);
		r_list_free (result);
		return false;
	}

	r_list_sort (block_list, (RListComparator)bbCMP);

	while (block_list->length > 0) {
		block = r_list_pop (block_list);
		if (!block) {
			eprintf ("Failed to get next block from list\n");
			continue;
		}
		if (r_cons_is_breaked ()) {
			break;
		}

		if (block_list->length > 0) {
			bb_t *next_block = (bb_t*) r_list_iter_get_data (block_list->tail);
			if (!next_block) {
				eprintf ("No next block to compare with!\n");
			}

			// current block is just a split block
			if (block->start == next_block->start && block->end == UT64_MAX) {
				if (block->type != CALL && next_block->type != CALL) {
					next_block->reached = block->reached + 1;
				}
				free (block);
				continue;
			}

			// block and next_block share the same start so we copy the
			// contenct of the block into the next_block and skip the current one
			if (block->start == next_block->start && next_block->end == UT64_MAX) {
				if (next_block->type != CALL)  {
					block->reached += 1;
				}
				*next_block = *block;
				free (block);
				continue;
			}

			if (block->end < UT64_MAX && next_block->start < block->end && next_block->start > block->start) {
				if (next_block->jump == UT64_MAX) {
					next_block->jump = block->jump;
				}

				if (next_block->fail == UT64_MAX) {
					next_block->fail = block->fail;
				}

				next_block->end = block->end;
				block->end = next_block->start;
				block->jump = next_block->start;
				block->fail = UT64_MAX;
				next_block->type = block->type;
				if (next_block->type != CALL)  {
					next_block->reached += 1;
				}
			}
		}

		sdb_ptr_set (sdb, sdb_fmt (0, "bb.0x%08"PFMT64x, block->start), block, 0);
		r_list_append (result, block);
	}

	// finally search for functions
	// we simply assume that non reached blocks or called blocks
	// are functions
	r_list_foreach (result, iter, block) {
		if (r_cons_is_breaked ()) {
			break;
		}
		if (block && (block->reached == 0 || block->called >= 1)) {
			fcn_t* current_function = fcnNew (block);
			RStack *stack = r_stack_new (100);
			bb_t *jump = NULL;
			bb_t *fail = NULL;
			bb_t *cur = NULL;

			if (!r_stack_push (stack, (void*)block)) {
				eprintf ("Failed to push initial block\n");
			}

			while (!r_stack_is_empty (stack)) {
				cur = (bb_t*) r_stack_pop (stack);
				if (!cur) {
					continue;
				}
				sdb_num_set (sdb, Fhandled(cur->start), 1, 0);
				if (cur->score < 0) {
					break;
				}
				// we ignore negative blocks
				if ((st64)(cur->end - cur->start) < 0) {
					break;
				}

				fcnAddBB (current_function, cur);

				if (cur->jump < UT64_MAX && !sdb_num_get (sdb, Fhandled(cur->jump), NULL)) {
					jump = sdb_ptr_get (sdb, sdb_fmt (0, "bb.0x%08"PFMT64x, cur->jump), NULL);
					if (!jump) {
						eprintf ("Failed to get jump block at 0x%"PFMT64x"\n", cur->jump);
						continue;
					}
					if (!r_stack_push (stack, (void*)jump)) {
						eprintf ("Failed to push jump block to stack\n");
					}
				}

				if (cur->fail < UT64_MAX && !sdb_num_get (sdb, Fhandled(cur->fail), NULL)) {
					fail = sdb_ptr_get (sdb, sdb_fmt (0, "bb.0x%08" PFMT64x, cur->fail), NULL);
					if (!fail) {
						eprintf ("Failed to get fail block at 0x%"PFMT64x"\n", cur->fail);
						continue;
					}
					if (!r_stack_push (stack, (void*)fail)) {
						eprintf ("Failed to push jump block to stack\n");
					}
				}
			}

			// function creation complete
			if (checkFunction (current_function)) {
				if (input[0] == '*') {
					printFunctionCommands (core, current_function, NULL);
				} else {
					createFunction (core, current_function, NULL);
				}
			}

			r_stack_free (stack);
			fcnFree (current_function);
		}
	}

	sdb_free (sdb);
	r_list_free (result);
	r_list_free (block_list);
	return true;
}