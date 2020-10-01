#include <rz_types.h>
#include <rz_util.h>
#include <rz_anal.h>

/*	shared internal state of the subgraph generating functions	*/

typedef struct esil_cfg_generator_t {
	RzAnalEsil *esil;
	union {
		RStack *ifelse;
		RStack *vals;
	};
	// union for semantic purposes
	RContRBTree *blocks;
	// consider moving this to cfg? well, yes and no.
	// making Graph nodes fast available in RzAnalEsilCFG is great idea
	// A balanced tree is only best solution, if we want to store and lookup intervals
	// We need to look for intervals, so that we can resolve goto destinations INSIDE of a cpu-instruction
	// After an instruction got graphed, we only need their entry node (nodes with first == {xxx, 0 })
	// So after graphing an instruction, the blocks-tree should be cleared (don't free the content)
	// 	and nodes with first == {xxx, 0} should be stored in an sdb or similar in cfg, with xxx as key
	RzAnalEsilCFG *cfg;
	RGraphNode *cur;
	// current graph-node, so that I don't have to abuse cfg->start
	RIDStorage *atoms;
	// is this needed
	ut64 off;
	// is this needed
} EsilCfgGen;

// esil has support for multiple elses,
// so we need these cookies on the ifelse-stack to keep track of things:
// when entering an if-block the parent is set as the else_block
// when entering an else block it is set as else_block, if is_else is false, other wise as if_block
// when entering an else block is_else flips
typedef struct esil_cfg_scope_cookie_t {
	RGraphNode *if_block;
	RGraphNode *else_block;
	bool is_else;
} EsilCfgScopeCookie;

typedef enum {
	ESIL_VAL_CONST,
	ESIL_VAL_REG,
	ESIL_VAL_RESULT
} EsilValType;

typedef struct esil_value_t {
	ut64 val; //should be a union, but for goto-analysis ut64 is fine
	EsilValType type;
} EsilVal;

/*	HELPERS 	*/

static char *condrets_strtok(char *str, const char tok) {
	if (!str) {
		return NULL;
	}
	ut32 i = 0;
	while (1 == 1) {
		if (!str[i]) {
			break;
		}
		if (str[i] == tok) {
			str[i] = '\0';
			return &str[i + 1];
		}
		i++;
	}
	return NULL;
}

RzAnalEsilOp *esil_get_op (RzAnalEsil *esil, const char *op) {
	rz_return_val_if_fail (R_STR_ISNOTEMPTY (op) && esil && esil->ops, NULL);
	return ht_pp_find (esil->ops, op, NULL);
}

// this little thot atomizes an esil expressions by splitting it on ','
static void esil_expr_atomize(RIDStorage *atoms, char *expr) {
	ut32 forget_me;
	for (
		; !!expr && rz_id_storage_add (atoms, expr, &forget_me);
		expr = condrets_strtok (expr, ',')) {
	}
}

static void _free_bb_cb(void *data) {
	RzAnalEsilBB *bb = (RzAnalEsilBB *)data;
	free (bb->expr);
	free (bb);
}

// REMINDER: generating the block content needs to prepend setting the program counter
// rz_anal_esil_cfg_op does this ^, use it whenever generating cfg from op

// this nasty function is an insert-compare for RGraphNodes that contain RzAnalEsilBB
static int _graphnode_esilbb_insert_cmp(void *incoming, void *in, void *user) {
	RGraphNode *incoming_gnode = (RGraphNode *)incoming;
	RGraphNode *in_gnode = (RGraphNode *)in;
	RzAnalEsilBB *incoming_bb = (RzAnalEsilBB *)incoming_gnode->data;
	RzAnalEsilBB *in_bb = (RzAnalEsilBB *)in_gnode->data;

	// RzAnalEsilBBs have the nice property, that they cannot intersect,
	// so just comparing first and first should be fine for inserting
#if 0
	return incoming_bb->first - in_bb->first;
#endif
	// We MUST NOT use direct subtraction here, since st64 vs st32 can break the tree
	// be careful and check by compares
	if (incoming_bb->first.off < in_bb->first.off) {
		return -1;
	}
	if (incoming_bb->first.off > in_bb->first.off) {
		return 1;
	}
	// ok, so upper 64 msb are equal, now use the lower 16 lsb
	return incoming_bb->first.idx - in_bb->first.idx;
}

static int _graphnode_esilbb_find_cmp(void *incoming, void *in, void *user) {
	RzAnalEsilEOffset *find_me = (RzAnalEsilEOffset *)incoming;
	RGraphNode *in_gnode = (RGraphNode *)in;
	RzAnalEsilBB *in_bb = (RzAnalEsilBB *)in_gnode->data;
	// not sure if this is needed that way
	if (find_me->off < in_bb->first.off) {
		return -1;
	}
	if (find_me->off > in_bb->last.off) {
		return 1;
	}
	if (find_me->idx < in_bb->first.idx) {
		return -1;
	}
	if (find_me->idx > in_bb->last.idx) {
		return 1;
	}
	return 0;
}

static int _graphnode_delete_always_0_cmp(void *incoming, void *in, void *user) {
	EsilCfgGen *gen = (EsilCfgGen *)user;
	RGraphNode *delete_me = (RGraphNode *)in;
	RzAnalEsilBB *delete_me_bb = (RzAnalEsilBB *)delete_me->data;
	rz_graph_del_node (gen->cfg->g, delete_me);
	ut32 id;
	for (id = delete_me_bb->first.idx; id <= delete_me_bb->last.idx; id++) {
		rz_id_storage_delete (gen->atoms, id);
	}
	return 0;
}

void _handle_if_enter (EsilCfgGen *gen, ut32 id, const bool has_next) {
	if (!has_next) {
		return;
	}
	// TODO: check allocation here
	EsilCfgScopeCookie *cookie = R_NEW0 (EsilCfgScopeCookie);

	// get current bb
	//	RzAnalEsilBB *bb = (RzAnalEsilBB *)gen->cur->data;

	// create if-enter-bb
	RzAnalEsilBB *entered_bb = R_NEW0 (RzAnalEsilBB);
	entered_bb->first.off = entered_bb->last.off = gen->off;
	entered_bb->first.idx = entered_bb->last.idx = id + 1;
	entered_bb->enter = R_ANAL_ESIL_BLOCK_ENTER_TRUE;

	// create if-entered-graph-node
	RGraphNode *entered_node = rz_graph_add_node (gen->cfg->g, entered_bb);
	entered_node->free = _free_bb_cb;
	rz_rbtree_cont_insert (gen->blocks, entered_node, _graphnode_esilbb_insert_cmp, NULL);

	// add edge from entering node to entered node
	rz_graph_add_edge (gen->cfg->g, gen->cur, entered_node);

	// push scope-cookie
	cookie->if_block = entered_node;
	cookie->else_block = gen->cur;
	rz_stack_push (gen->ifelse, cookie);
	gen->cur = entered_node;
}

void _handle_else_enter (EsilCfgGen *gen, ut32 id, const bool has_next) {
	if (!has_next || rz_stack_is_empty (gen->ifelse)) {
		// no cookie no todo
		return;
	}
	EsilCfgScopeCookie *cookie = (EsilCfgScopeCookie *)rz_stack_peek (gen->ifelse);

	// create if-enter-bb
	RzAnalEsilBB *entered_bb = R_NEW0 (RzAnalEsilBB);
	entered_bb->first.off = entered_bb->last.off = gen->off;
	entered_bb->first.idx = entered_bb->last.idx = id + 1;

	// create if-entered-graph-node
	RGraphNode *entered_node = rz_graph_add_node (gen->cfg->g, entered_bb);
	entered_node->free = _free_bb_cb;
	rz_rbtree_cont_insert (gen->blocks, entered_node, _graphnode_esilbb_insert_cmp, NULL);

	if (cookie->is_else) {
		entered_bb->enter = R_ANAL_ESIL_BLOCK_ENTER_TRUE;
		rz_graph_add_edge (gen->cfg->g, cookie->if_block, entered_node);
		cookie->if_block = entered_node;
		cookie->else_block = gen->cur;
		cookie->is_else = false;
	} else {
		entered_bb->enter = R_ANAL_ESIL_BLOCK_ENTER_FALSE;
		rz_graph_add_edge (gen->cfg->g, cookie->else_block, entered_node);
		cookie->else_block = entered_node;
		cookie->if_block = gen->cur;
		cookie->is_else = true;
	}
	gen->cur = entered_node;
}

void _handle_fi_leave (EsilCfgGen *gen, ut32 id, const bool has_next) {
	EsilCfgScopeCookie *cookie = rz_stack_pop (gen->ifelse);
	if (!cookie) {
		// no if, no fi todo
		return;
	}

	RzAnalEsilBB *cur_bb = (RzAnalEsilBB *)gen->cur->data;
	// this block is not executed when the if or else block is empty
	if (memcmp (&cur_bb->first, &cur_bb->last, sizeof (RzAnalEsilEOffset))) {
		// TODO: add some thoughts in comments here
		cur_bb->last.idx--;
		RzAnalEsilBB *leaving_bb = R_NEW0 (RzAnalEsilBB);
		leaving_bb->first.off = leaving_bb->last.off = gen->off;
		leaving_bb->first.idx = leaving_bb->last.idx = id;
		RGraphNode *leaving_node = rz_graph_add_node (gen->cfg->g, leaving_bb);
		leaving_node->free = _free_bb_cb;
		rz_graph_add_edge (gen->cfg->g, gen->cur, leaving_node);
		rz_rbtree_cont_insert (gen->blocks, leaving_node, _graphnode_esilbb_insert_cmp, NULL);
		gen->cur = leaving_node;
		cur_bb = leaving_bb;
	}
	rz_graph_add_edge (gen->cfg->g, cookie->is_else ? cookie->if_block : cookie->else_block, gen->cur);
	free (cookie);
}

// this function handles '?{','}{’ and '}'
// return type should probably be a bool, but idk
void _handle_control_flow_ifelsefi (EsilCfgGen *gen, char *atom, ut32 id) {
	// we're probably going to see more ?{ and }, than }{
	// so checking against ?{ and } befor }{ is therefor better for perf (lololol)
	if (!strcmp (atom, "?{")) {
		_handle_if_enter (gen, id, !!rz_id_storage_get (gen->atoms, id + 1));
		return;
	}
	if (!strcmp (atom, "}")) {
		_handle_fi_leave (gen, id, !!rz_id_storage_get (gen->atoms, id + 1));
		return;
	}
	if (!strcmp (atom, "}{")) {
		_handle_else_enter (gen, id, !!rz_id_storage_get (gen->atoms, id + 1));
	}
}

// this little function is expected to generate a subgraph with most nodes in it
// but not all edges. It's expected to handle if, else and fi
bool _round_0_cb (void *user, void *data, ut32 id) {
	EsilCfgGen *gen = (EsilCfgGen *)user;
	char *atom = (char *)data;
	RzAnalEsilBB *bb = (RzAnalEsilBB *)gen->cur->data;
	RzAnalEsilOp *op = esil_get_op (gen->esil, atom);
	bb->last.idx = (ut16)id;
	if (op && op->type == R_ANAL_ESIL_OP_TYPE_CONTROL_FLOW) {
		_handle_control_flow_ifelsefi (gen, atom, id);
	}
	return true;
}

RGraphNode *_common_break_goto (EsilCfgGen *gen, ut32 id) {
	RzAnalEsilEOffset off = { gen->off, (ut16)id };
	RGraphNode *gnode = rz_rbtree_cont_find (gen->blocks, &off, _graphnode_esilbb_find_cmp, NULL);
	RzAnalEsilBB *bb = (RzAnalEsilBB *)gnode->data;
	if (id != bb->last.idx) {
		RzAnalEsilBB *next_bb = R_NEW0 (RzAnalEsilBB);
		// split blocks
		next_bb->first.off = gen->off;
		next_bb->first.idx = id + 1;
		next_bb->last = bb->last;
		bb->last.idx = id;
		RGraphNode *next_gnode = rz_graph_node_split_forward (gen->cfg->g, gnode, next_bb);
		// TODO: implement node_split in graph api
		rz_rbtree_cont_insert (gen->blocks, next_gnode, _graphnode_esilbb_insert_cmp, NULL);
	} else {
		RzListIter *iter, *ator;
		RGraphNode *node;
		// TODO: improve perf here
		rz_list_foreach_safe (gnode->out_nodes, iter, ator, node) {
			rz_graph_del_edge (gen->cfg->g, gnode, node);
		}
	}
	return gnode;
	// rz_graph_add_edge(gen->cfg->g, gnode, gen->cfg->end);
}

void _handle_break (EsilCfgGen *gen, ut32 id) {
	rz_graph_add_edge (gen->cfg->g, _common_break_goto (gen, id), gen->cfg->end);
}

void _handle_goto (EsilCfgGen *gen, ut32 idx) {
	RGraphNode *gnode = _common_break_goto (gen, idx);
	RzAnalEsilBB *bb = (RzAnalEsilBB *)gnode->data;
	// so what we're doing here is emulating this block with a certain degree of abstraction:
	// no reg-access
	// no io-access
	// stack-movents
	// maybe arithmetic stack operations
	//
	// we need to figure out the goto destination
	// ex: "a,b,=,12,GOTO" => goto dst is 12
	// ex: "42,a,4,+,b,=,GOTO" => goto dst is 42
	//
	// TODO: also handle "2,14,+,GOTO" in later versions
	ut16 id;
	// bb->last.idx is the GOTO operation itself, we do not reach this in the loop
	for (id = bb->first.idx; id < bb->last.idx; id++) {
		char *atom = (char *)rz_id_storage_get (gen->atoms, (ut32)id);
		RzAnalEsilOp *op = esil_get_op (gen->esil, atom);
		if (op) {
			ut32 j;
			for (j = 0; j < op->pop; j++) {
				free (rz_stack_pop (gen->vals));
			}
			for (j = 0; j < op->push; j++) {
				EsilVal *val = R_NEW (EsilVal);
				val->type = ESIL_VAL_RESULT;
				rz_stack_push (gen->vals, val);
			}
		} else {
			EsilVal *val = R_NEW (EsilVal);
			if (rz_reg_get (gen->esil->anal->reg, atom, -1)) {
				val->type = ESIL_VAL_REG;
			} else {
				val->type = ESIL_VAL_CONST;
				val->val = rz_num_get (NULL, atom);
			}
			rz_stack_push (gen->vals, val);
		}
	}
	EsilVal *v = rz_stack_pop (gen->vals);
	if (!v || v->type != ESIL_VAL_CONST) {
		free (v);
		eprintf ("Cannot resolve GOTO dst :(\n");
		goto beach;
	}

	// get the node to the corresponding GOTO destination
	RzAnalEsilEOffset dst_off = { gen->off, (ut16)v->val };
	RGraphNode *dst_node = rz_rbtree_cont_find (gen->blocks, &dst_off, _graphnode_esilbb_find_cmp, NULL);
	if (!dst_node) {
		// out-of-bounds
		// check if this works
		dst_node = gen->cfg->end;
	} else {
		RzAnalEsilBB *dst_bb = (RzAnalEsilBB *)dst_node->data;
		if (dst_bb->first.idx != v->val) {
			RzAnalEsilBB *split_bb = R_NEW0 (RzAnalEsilBB);
			split_bb[0] = dst_bb[0];
			dst_bb->last.idx = v->val - 1;
			split_bb->first.idx = v->val;
			RGraphNode *split = rz_graph_node_split_forward (gen->cfg->g, dst_node, split_bb);
			rz_graph_add_edge (gen->cfg->g, dst_node, split);
			dst_node = split;
		}
	}

	rz_graph_add_edge (gen->cfg->g, gnode, dst_node);
beach:
	while (!rz_stack_is_empty (gen->vals)) {
		free (rz_stack_pop (gen->vals));
	}
}

bool _round_1_cb (void *user, void *data, ut32 id) {
	EsilCfgGen *gen = (EsilCfgGen *)user;
	char *atom = (char *)data;
	RzAnalEsilOp *op = esil_get_op (gen->esil, atom);
	if (op && op->type == R_ANAL_ESIL_OP_TYPE_CONTROL_FLOW) {
		if (!strcmp ("BREAK", atom)) {
			_handle_break (gen, id);
		}
		if (!strcmp ("GOTO", atom)) {
			_handle_goto (gen, id);
		}
	}
	return true;
}

void _round_2_cb (RGraphNode *n, RGraphVisitor *vi) {
	RzAnalEsilBB *bb = (RzAnalEsilBB *)n->data;
	EsilCfgGen *gen = (EsilCfgGen *)vi->data;
	RStrBuf *buf = rz_strbuf_new ((char *)rz_id_storage_get (gen->atoms, bb->first.idx));
	rz_strbuf_append (buf, ",");
	ut32 id;
	for (id = bb->first.idx + 1; id <= bb->last.idx; id++) {
		// use rz_id_storage_take here to start fini for the atoms
		rz_strbuf_appendf (buf, "%s,", (char *)rz_id_storage_take (gen->atoms, id));
	}
	bb->expr = strdup (rz_strbuf_get (buf));
	rz_strbuf_free (buf);
	rz_rbtree_cont_delete (gen->blocks, n, _graphnode_esilbb_insert_cmp, NULL);
}

// this function takes a cfg, an offset and an esil expression
// concatinates to already existing graph.
// Also expects RIDStorage atoms and RContRBTree to be allocate in prior of the call
static RzAnalEsilCFG *esil_cfg_gen(RzAnalEsilCFG *cfg, RzAnal *anal, RIDStorage *atoms, RContRBTree *blocks, RStack *stack, ut64 off, char *expr) {
	// consider expr as RStrBuf, so that we can sanitze broken esil
	// (ex: "b,a,+=,$z,zf,:=,7,$c,cf,:=,zf,?{,1,b,+=,cf,?{,3,a,-=" =>
	// 	"b,a,+=,$z,zf,:=,7,$c,cf,:=,zf,?{,1,b,+=,cf,?{,3,a,-=,},}")

	// allocate some stuff
	char *_expr = strdup (expr);
	if (!_expr) {
		return cfg; //NULL?
	}
	RzAnalEsilBB *end_bb = R_NEW0 (RzAnalEsilBB);
	if (!end_bb) {
		free (_expr);
		return cfg;
	}
	RGraphNode *start, *end = rz_graph_add_node (cfg->g, end_bb);
	if (!end) {
		free (end_bb);
		free (_expr);
		return cfg;
	}
	end->free = _free_bb_cb;

	esil_expr_atomize (atoms, _expr);

	// previous expression's post-dominator is the current expression starting point
	//
	// MUST NOT use cfg->start as starting point of subgraph,
	// since it marks the start of the whole graph
	//
	// cpu-instruction starts at this node
	//
	// without information about the outside cfg, we CANNOT merge cpu-instructions

	RzAnalEsilBB *bb = (RzAnalEsilBB *)cfg->end->data;

	end_bb->expr = bb->expr;
	// FIXME: use end_bb here
	bb->expr = NULL;
	bb->first.off = bb->last.off = off;
	bb->first.idx = bb->last.idx = 0;
	start = cfg->end;

	EsilCfgGen gen = { anal->esil, { stack }, blocks, cfg, start, atoms, off };
	cfg->end = end;
	// create an edge from cur to end?
	// Well yes, but no. Would be great to do this,
	// but rgraph api is slow af on node delition. Be careful instead

	// We created a new graph node above, which is going to be the post-dominator
	// of the subgraph, that we are going to add to the existing graph.
	// The post-dominator of the previous added subgraph is the starting node here.
	// We add this to the block-tree
	rz_rbtree_cont_insert (blocks, gen.cur, _graphnode_esilbb_insert_cmp, NULL);

	// end of the initial setup, next generate blocks and insert them in the tree

	// round 0 adds a subgraph from if, else and fi
	rz_id_storage_foreach (atoms, _round_0_cb, &gen);
	// make cfg->end effective post-dominator
	rz_graph_add_edge (cfg->g, gen.cur, cfg->end);
	{
		// stack unwinding
		EsilCfgScopeCookie *cookie;
		while ((cookie = rz_stack_pop (stack))) {
			rz_graph_add_edge (cfg->g,
				cookie->is_else ? cookie->if_block : cookie->else_block, cfg->end);
			free (cookie);
		}
	}

	// next do round 1: split blocks from GOTOs and BREAKs
	rz_id_storage_foreach (atoms, _round_1_cb, &gen);

	// next do dfs:
	//  - remove each node from blocks-tree, that can be reached by a dfs path
	//  - when removing a node from block-tree, synthesize node->bb->expr with RStrBuf
	{
		// dfs walk removes used atoms
		RGraphVisitor vi = { _round_2_cb, NULL, NULL, NULL, NULL, &gen };
		rz_graph_dfs_node (cfg->g, start, &vi);
	}
	// this loop removes unused atoms
	do {
	} while (blocks->root && rz_rbtree_cont_delete (blocks, NULL, _graphnode_delete_always_0_cmp, &gen));

	free (_expr);
	return cfg;
}

RZ_API RzAnalEsilCFG *rz_anal_esil_cfg_new(void) {
	RzAnalEsilCFG *cf = R_NEW0 (RzAnalEsilCFG);
	if (cf) {
		RzAnalEsilBB *p = R_NEW0 (RzAnalEsilBB);
		if (!p) {
			free (cf);
			return NULL;
		}
		p->expr = strdup ("end");
		if (!p->expr) {
			free (p);
			free (cf);
			return NULL;
		}
		cf->g = rz_graph_new ();
		if (!cf->g) {
			free (p->expr);
			free (p);
			free (cf);
			return NULL;
		}
		cf->start = cf->end = rz_graph_add_node (cf->g, p);
		// end node is always needed as post-dominator
		// idea here is to split the initial one node graph in the node
		if (!cf->end) {
			free (p->expr);
			free (p);
			rz_graph_free (cf->g);
			free (cf);
			return NULL;
		}
		if (cf->g->nodes) {
			cf->end->free = _free_bb_cb;
		}
	}
	return cf;
}

// this little function takes a cfg, an offset and an esil expression
// concatinates to already existing graph
RZ_API RzAnalEsilCFG *rz_anal_esil_cfg_expr(RzAnalEsilCFG *cfg, RzAnal *anal, const ut64 off, char *expr) {
	if (!anal || !anal->esil) {
		return NULL;
	}
	RStack *stack = rz_stack_new (4);
	if (!stack) {
		return NULL;
	}
	RContRBTree *blocks = rz_rbtree_cont_new ();
	if (!blocks) {
		rz_stack_free (stack);
		return NULL;
	}
	RIDStorage *atoms = rz_id_storage_new (0, 0xfffe);
	if (!atoms) {
		rz_stack_free (stack);
		rz_rbtree_cont_free (blocks);
		return NULL;
	}
	RzAnalEsilCFG *cf = cfg ? cfg : rz_anal_esil_cfg_new ();
	if (!cf) {
		rz_stack_free (stack);
		rz_id_storage_free (atoms);
		rz_rbtree_cont_free (blocks);
		return NULL;
	}
	RzAnalEsilCFG *ret = esil_cfg_gen (cf, anal, atoms, blocks, stack, off, expr);
	rz_stack_free (stack);
	rz_id_storage_free (atoms);
	rz_rbtree_cont_free (blocks);
	return ret;
}

RZ_API RzAnalEsilCFG *rz_anal_esil_cfg_op(RzAnalEsilCFG *cfg, RzAnal *anal, RzAnalOp *op) {
	if (!op || !anal || !anal->reg || !anal->esil) {
		return NULL;
	}
	RzAnalEsilBB *glue_bb = R_NEW0 (RzAnalEsilBB);
	if (!glue_bb) {
		eprintf ("Couldn't allocate glue_bb\n");
		return NULL;
	}
	RStrBuf *glue = rz_strbuf_new ("");
	if (!glue) {
		free (glue_bb);
		eprintf ("Couldn't allocate glue\n");
		return NULL;
	}
	const char *pc = rz_reg_get_name (anal->reg, R_REG_NAME_PC);
	rz_strbuf_setf (glue, "0x%" PFMT64x ",%s,:=,", op->addr + op->size, pc);
	glue_bb->expr = strdup (rz_strbuf_get (glue));
	rz_strbuf_free (glue);
	if (!glue_bb->expr) {
		free (glue_bb);
		eprintf ("Couldn't strdup\n");
		return NULL;
	}
	glue_bb->enter = R_ANAL_ESIL_BLOCK_ENTER_GLUE;
	glue_bb->first.off = glue_bb->last.off = op->addr;
	glue_bb->first.idx = glue_bb->last.idx = 0;

	RzAnalEsilCFG *ret;

	if (!cfg) {
		ret = rz_anal_esil_cfg_expr (cfg, anal, op->addr, rz_strbuf_get (&op->esil));
		RGraphNode *glue_node = rz_graph_add_node (ret->g, glue_bb);
		glue_node->free = _free_bb_cb;
		rz_graph_add_edge (ret->g, glue_node, ret->start);
		ret->start = glue_node;
	} else {
		RGraphNode *glue_node = rz_graph_add_node (cfg->g, glue_bb);
		glue_node->free = _free_bb_cb;
		rz_graph_add_edge (cfg->g, cfg->end, glue_node);
		void *foo = cfg->end->data;
		cfg->end->data = glue_node->data;
		glue_node->data = foo;
		cfg->end = glue_node;
		ret = rz_anal_esil_cfg_expr (cfg, anal, op->addr, rz_strbuf_get (&op->esil));
	}
	return ret;
}

static void merge_2_blocks(RzAnalEsilCFG *cfg, RGraphNode *node, RGraphNode *block) {
	// merge node and block, block dies in this
	// block----->node ===> node
	if (node == cfg->end) {
		// do not merge the post-dominator
		return;
	}
	RzListIter *iter;
	RGraphNode *n;
	rz_list_foreach (block->in_nodes, iter, n) {
		rz_graph_add_edge (cfg->g, n, node);
	}
	RzAnalEsilBB *block_bb, *node_bb = (RzAnalEsilBB *)node->data;
	block_bb = (RzAnalEsilBB *)block->data;
	if ((block_bb->enter == R_ANAL_ESIL_BLOCK_ENTER_TRUE) || (block_bb->enter == R_ANAL_ESIL_BLOCK_ENTER_FALSE)) {
		node_bb->enter = block_bb->enter;
	} else {
		node_bb->enter = R_ANAL_ESIL_BLOCK_ENTER_NORMAL;
	}
	RStrBuf *buf = rz_strbuf_new (block_bb->expr);
	node_bb->first = block_bb->first;
	rz_graph_del_node (cfg->g, block);
	rz_strbuf_appendf (buf, "\n%s", node_bb->expr);
	free (node_bb->expr);
	node_bb->expr = strdup (rz_strbuf_get (buf));
	if (block == cfg->start) {
		cfg->start = node;
	}
}

// this shit is slow af, bc of foolish graph api
RZ_API void rz_anal_esil_cfg_merge_blocks(RzAnalEsilCFG *cfg) {
	if (!cfg || !cfg->g || !cfg->g->nodes) {
		return;
	}
	RzListIter *iter, *ator;
	RGraphNode *node;
	rz_list_foreach_safe (cfg->g->nodes, iter, ator, node) {
		if (rz_list_length (node->in_nodes) == 1) {
			RzAnalEsilBB *bb = (RzAnalEsilBB *)node->data;
			RGraphNode *top = (RGraphNode *)rz_list_get_top (node->out_nodes);
			// segfaults here ?
			if (!(top && bb->enter == R_ANAL_ESIL_BLOCK_ENTER_GLUE && (rz_list_length (top->in_nodes) > 1))) {
				RGraphNode *block = (RGraphNode *)rz_list_get_top (node->in_nodes);
				if (rz_list_length (block->out_nodes) == 1) {
					merge_2_blocks (cfg, node, block);
				}
			}
		}
	}
}

RZ_API void rz_anal_esil_cfg_free(RzAnalEsilCFG *cfg) {
	if (cfg && cfg->g) {
		rz_graph_free (cfg->g);
	}
	free (cfg);
}