#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "compiler.h"
#include "backend.h"

/*
 *	The core compiler thinks mostly in bytes. Whilst it understands
 *	pointer conversions and the like our stack offsets in arguments
 *	and locals are byte oriented (as we might pack byte variables).
 *
 *	In addition on the Nova our stack grows up through memory so all
 *	the argument offsets are negative upwards from the frame pointer
 *	allowing for the SAV frame (5 words)
 *
 *	TODO:
 *
 *	Add support for Nova4 (LDB STB)
 *	Add support for Mul/Div hardware
 *	Add support for Nova < 3 using helpers
 *
 *	Shift optimized and short helper mul/div constant
 *	Inline small left and unsigned right shifts
 *	Inline by 8 shifts
 *
 *	Compare optimizations. We can do better stuff
 *	for 0 based compares, for 1 and -1 compares which
 *	are all common, and for sign bit compares
 *
 *	Track contents of AC1 so we can avoid reloading constants
 *	(may be worth tracing AC0 too but less clear)
 *
 *	Byte LREF/LSTORE etc
 *	Long LREF/LSTORE etc
 *
 *	Floating point
 *
 *	Eclipse
 *	- Multiply/Divide/Halve/LEA/Immediate forms
 *	- ejsr, elda, esta
 *	- push/pop do 1-4 accumulators at a time
 *	- msp for stack cleanup
 *	- mffp and friends instead are memory 040/041
 *	- signed compare two ac
 */

#define BYTE(x)		(((unsigned)(x)) & 0xFF)
#define WORD(x)		(((unsigned)(x)) & 0xFFFF)

/*
 *	State for the current function
 */
static unsigned frame_len;	/* Number of bytes of stack frame */
static unsigned sp;		/* Stack pointer offset tracking */
static unsigned argbase;	/* Argument offset */
static unsigned unreachable;	/* Is code currently unreachable */

#define ARGBASE	10		/* 5 words (10 bytes) */

/* Chance to rewrite the tree from the top rather than none by node
   upwards. We will use this for 8bit ops at some point and for cconly
   propagation */
struct node *gen_rewrite(struct node *n)
{
	return n;
}

/*
 *	Byte sizes although we are a word machine
 */
static unsigned get_size(unsigned t)
{
	if (PTR(t))
		return 2;
	if (t == CSHORT || t == USHORT)
		return 2;
	if (t == CCHAR || t == UCHAR)
		return 1;
	if (t == CLONG || t == ULONG || t == FLOAT)
		return 4;
	if (t == CLONGLONG || t == ULONGLONG || t == DOUBLE)
		return 8;
	if (t == VOID)
		return 0;
	error("gs");
	return 0;
}

static unsigned get_stack_size(unsigned t)
{
	unsigned n = get_size(t);
	if (n == 1)
		return 2;
	return n;
}

#define T_NREF		(T_USER)		/* Load of C global/static */
#define T_CALLNAME	(T_USER+1)		/* Function call by name */
#define T_NSTORE	(T_USER+2)		/* Store to a C global/static */
#define T_LREF		(T_USER+3)		/* Ditto for local */
#define T_LSTORE	(T_USER+4)
#define T_LBREF		(T_USER+5)		/* Ditto for labelled strings or local static */
#define T_LBSTORE	(T_USER+6)
#define T_RREF		(T_USER+7)
#define T_RSTORE	(T_USER+8)
#define T_RDEREF	(T_USER+9)		/* *regptr */
#define T_REQ		(T_USER+10)		/* *regptr */

static void squash_node(struct node *n, struct node *o)
{
	n->value = o->value;
	n->val2 = o->val2;
	n->snum = o->snum;
	free_node(o);
}

static void squash_left(struct node *n, unsigned op)
{
	struct node *l = n->left;
	n->op = op;
	squash_node(n, l);
	n->left = NULL;
}

static void squash_right(struct node *n, unsigned op)
{
	struct node *r = n->right;
	n->op = op;
	squash_node(n, r);
	n->right = NULL;
}

static unsigned is_bytepointer(unsigned t)
{
	if (!PTR(t))
		return 0;
	t = BASE_TYPE(t) & ~UNSIGNED;
	if (t == CCHAR || t == VOID)
		return 1;
	return 0;
}

/*
 *	As a word machine our types matter for pointer conversions
 *	and we cannot blithly throw them away as most byte machines
 *	can. In the Nova case we need to shift it left or right between
 *	byte pointers (void, char *, unsigned char *) and other types
 */
static unsigned pointer_match(unsigned t1, unsigned t2)
{
	if (!PTR(t1) || !PTR(t2))
		return 0;
	if (is_bytepointer(t1) == is_bytepointer(t2))
		return 1;
	return 0;
}

/*
 *	Our chance to do tree rewriting. We don't do much
 *	at this point, but we do rewrite name references and function calls
 *	to make them easier to process.
 */
struct node *gen_rewrite_node(struct node *n)
{
	struct node *l = n->left;
	struct node *r = n->right;
	unsigned op = n->op;
	unsigned nt = n->type;

	/* TODO: implement derefplus as we can fold small values into
	   a deref pattern */

	/* Rewrite references into a load operation. Don't do this with
	   long. We can probably do long sanely later. Char is weird as
	   we then use byte pointers */
	if (nt == CSHORT || nt == USHORT || PTR(nt)) {
		if (op == T_DEREF) {
			if (r->op == T_LOCAL || r->op == T_ARGUMENT) {
				/* Offsets are in bytes, we are a word machine */
				/* Arguments are below the FP, variables above with
				   a 2 byte offset */
				if (r->op == T_ARGUMENT)
					r->value = -(argbase + r->value);
				else
					r->value += 2;
				/* Always word addressed */
				r->value /= 2;
				squash_right(n, T_LREF);
				return n;
			}
			if (r->op == T_REG) {
				squash_right(n, T_RREF);
				return n;
			}
			if (r->op == T_NAME) {
				squash_right(n, T_NREF);
				return n;
			}
			if (r->op == T_LABEL) {
				squash_right(n, T_LBREF);
				return n;
			}
		}
		if (op == T_EQ) {
			if (l->op == T_NAME) {
				squash_left(n, T_NSTORE);
				return n;
			}
			if (l->op == T_LABEL) {
				squash_left(n, T_LBSTORE);
				return n;
			}
			if (l->op == T_LOCAL || l->op == T_ARGUMENT) {
				if (l->op == T_ARGUMENT)
					l->value = -(argbase + l->value);
				else
					l->value += 2;
				l->value /= 2;	/* Word machine */
				squash_left(n, T_LSTORE);
				return n;
			}
			if (l->op == T_REG) {
				squash_left(n, T_RSTORE);
				return n;
			}
		}
	}
	/* Eliminate casts for sign, pointer conversion or same */
	if (op == T_CAST) {
		if (nt == r->type || (nt ^ r->type) == UNSIGNED ||
		 (pointer_match(nt, r->type))) {
			free_node(n);
			r->type = nt;
			return r;
		}
	}
	/* Rewrite function call of a name into a new node so we can
	   turn it easily into call xyz */
	if (op == T_FUNCCALL && r->op == T_NAME && PTR(r->type) == 1) {
		n->op = T_CALLNAME;
		n->snum = r->snum;
		n->value = r->value;
		free_node(r);
		n->right = NULL;
	}
	return n;
}

/* Export the C symbol */
void gen_export(const char *name)
{
	printf("	.export _%s\n", name);
}

void gen_segment(unsigned s)
{
	switch(s) {
	case A_CODE:
		printf("\t.code\n");
		break;
	case A_DATA:
		printf("\t.data\n");
		break;
	case A_LITERAL:
		printf("\t.literal\n");
		break;
	case A_BSS:
		printf("\t.bss\n");
		break;
	default:
		error("gseg");
	}
}

void gen_prologue(const char *name)
{
	unreachable = 0;
	printf("_%s:\n", name);
}

static void repeated_op(unsigned n, char *op)
{
	while(n--)
		printf("\t%s\n", op);
}

/* Generate the stack frame */
void gen_frame(unsigned size, unsigned aframe)
{
	frame_len = size;
	argbase = ARGBASE;
	sp = 0;
	/* Remember the stack grows upwards so values are negative offsets */
	printf("\tsav\n");
	printf("\tisz 0,3\n");	/* Will never skip */
	if (size == 0)
		return;
	if (size >= 5) {
		printf("\tmfsp 1\n");
		printf("\tlda 0,2,1\n");
		printf("\tadd 0,1,skp\n");
		printf("\t.word %u\n", size / 2);
		printf("\tmtsp 1\n");
	} else
		repeated_op(size / 2, "psha 0");
	printf(";\n");
}

/* The return restores all the registers so we have to patch the stack frame */
void gen_epilogue(unsigned size, unsigned argsize)
{
	if (sp != 0)
		error("sp");
	if (unreachable)
		return;
	if (!(func_flags & F_VOIDRET))
		printf("\tsta 1,-3,3\n");
	printf("\tret\n");
	unreachable = 1;
}

void gen_label(const char *tail, unsigned n)
{
	unreachable = 0;
	printf("L%d%s:\n", n, tail);
}

unsigned gen_exit(const char *tail, unsigned n)
{
	/* TODO: we need an ejmp that works out if it's in range as it often
	   will be doable relative */
	printf("\tjmp @1,1\n");
	printf("\t.word L%d%s\n", n, tail);
	unreachable = 1;
	return 0;
}

void gen_jump(const char *tail, unsigned n)
{
	printf("\tjmp @1,1\n");
	printf("\t.word L%d%s\n", n, tail);
}

void gen_jfalse(const char *tail, unsigned n)
{
	/* TODO we need a self expanding jump with value hiding */
	printf("\tjsr @__jf,0\n");
	printf("\t.word L%d%s\n", n, tail);
}

void gen_jtrue(const char *tail, unsigned n)
{
	printf("\tjsr @__jt,0\n");
	printf("\t.word L%d%s\n", n, tail);
}

void gen_switch(unsigned n, unsigned type)
{
	printf("\tjsr @__switch,0\n");
	printf("\t.word Sw%d\n", n);
	unreachable = 1;
	/* Although we jsr that's just to pass the table ptr */
}

void gen_switchdata(unsigned n, unsigned size)
{
	printf("Sw%d:\n", n);
	printf("\t.word %d\n", size);
}

void gen_case_label(unsigned tag, unsigned entry)
{
	unreachable = 0;
	printf("Sw%d_%d:\n", tag, entry);
}

void gen_case_data(unsigned tag, unsigned entry)
{
	printf("\t.word Sw%d_%d\n", tag, entry);
}

void gen_helpcall(struct node *n)
{
	/* TODO need to figure out what is indirected this way and what
	   is done jsr @1,1 style */
	printf("\tjsr @__");
}

void gen_helptail(struct node *n)
{
	printf(",0");
}

void gen_helpclean(struct node *n)
{
}

void gen_data_label(const char *name, unsigned align)
{
	printf("_%s:\n", name);
}

void gen_space(unsigned value)
{
	/* Word based */
	printf("\t.ds %d\n", (value + 1) / 2);
}

void gen_text_data(unsigned n)
{
	printf("\t.word T%d\n", n);
}

void gen_literal(unsigned n)
{
	if (n)
		printf("T%d:\n", n);
}

void gen_name(struct node *n)
{
	printf("\t.word _%s+%d\n", namestr(n->snum), WORD(n->value));
}

/* FIXME: we will need to add .byte and alignment padding to the
   assembler for char arrays. This is now OK to do as the asm/ld think
   in bytes for word machines with byte pointers */
void gen_value(unsigned type, unsigned long value)
{
	unsigned v = value & 0xFFFF;
	if (PTR(type)) {
		if (is_bytepointer(type))
			printf("\t.byteptr %u\n", v);
		else
			printf("\t.word %u\n", v);
		return;
	}
	switch (type) {
		/* Bytes alone are word aligned on the left of the word */
	case CCHAR:
	case UCHAR:
		/* FIXME */
		printf("\t.word %u\n", (v << 8) & 0xFF00);
		break;
	case CSHORT:
	case USHORT:
		printf("\t.word %u\n", v);
		break;
	case CLONG:
	case ULONG:
	case FLOAT:
		/* We are big endian - software choice */
		printf("\t.word %u\n", (unsigned) ((value >> 16) & 0xFFFF));
		printf("\t.word %u\n", v);
		break;
	default:
		error("unsuported type");
	}
}

/* Byte constants for helpers are written as words */
void gen_wvalue(unsigned type, unsigned long value)
{
	unsigned v = value & 0xFFFF;
	if (PTR(type)) {
		if (is_bytepointer(type))
			printf("\t.byteptr %u\n", v);
		else
			printf("\t.word %u\n", v);
		return;
	}
	switch (type) {
		/* Bytes alone are word aligned on the left of the word */
	case CCHAR:
	case UCHAR:
	case CSHORT:
	case USHORT:
		printf("\t.word %u\n", v);
		break;
	case CLONG:
	case ULONG:
	case FLOAT:
		/* We are big endian - software choice */
		printf("\t.word %u\n", (unsigned) ((value >> 16) & 0xFFFF));
		printf("\t.word %u\n", v);
		break;
	default:
		error("unsuported type");
	}
}

void gen_start(void)
{
	printf("\t.code\n");
}

void gen_end(void)
{
}

void gen_tree(struct node *n)
{
	codegen_lr(n);
	printf(";\n");
}

unsigned gen_push(struct node *n)
{
	/* Our push will put the object on the stack, so account for it */
	unsigned s = get_stack_size(n->type);
	sp += s / 2;
	if (s == 4) {
		printf("\tlda 0,__hireg, 0\n");
		printf("\tpsha 0\n");
	} else
		printf("\tpsha 1\n");
	printf(";\n");
	return 1;
}

static unsigned gen_constant(unsigned r, int16_t v)
{
	/* TODO: other values - to begin with byte swap forms of these */
	switch(v) {
	case 0:
		printf("\tsub %u,%u\n", r, r);
		return 1;
	case 1:
		printf("\tsubzl %u,%u\n", r, r);
		return 1;
	case 2:
		printf("\tsub %u,%u\n", r, r);
		printf("\tinczl %u,%u\n", r, r);
		return 1;
	case 3:
		printf("\tsub %u,%u\n", r, r);
		printf("\tincol %u,%u\n", r, r);
		return 1;
	case -1:
		printf("\tadc %u,%u\n", r, r);
		return 1;
	case -2:
		printf("\tadcz %u,%u\n", r, r);
		return 1;
	}
	return 0;
}

/*
 *	True if we can load ac0 with the value we need without trashing
 *	AC1. This lets us avoid a lot of the pushing and popping we would
 *	otherwise do in the code generation. The more we can add to this
 *	the better. Right now we just do some ops but we could look at
 *	implementable subtrees even - eg deref of thing we can do or
 *	maths ops via AC2 on AC0
 */
static unsigned can_load_ac(struct node *n)
{
	/* Start with simple stuff */
	unsigned s = get_size(n->type);
	if (s != 2)
		return 0;
	switch(n->type) {
	case T_LOCAL:
	case T_LREF:
	case T_NAME:
	case T_LABEL:
	case T_CONSTANT:
	case T_NREF:
	case T_LBREF:
		return 1;
	}
	return 0;
}
/*
 *	The actual helper can do more than can_load_ac but must either
 *	generate all the code or none. This allows some ops to try and
 *	see what is possible if they can recover from a "no" answer
 *
 *	TODO: teach it the constants we can magic up in 0
 */
static unsigned load_ac(unsigned ac, struct node *n)
{
	unsigned v = n->value;
	int16_t d = v;

	switch(n->op) {
	case T_ARGUMENT:
		/* Stack grows upward and our offsets are in words */
		/* Except for bytes, then our addresses for locals are
		   byte pointers! */
		d = -(argbase + d);	/* Bytes */
		if (is_bytepointer(n->type)) {
			/* Convert to byte pointer */
			printf("\tmovzl 3,%u\n", ac);
			d++;	/* Arguments are stacked as words so the
				   value is 1 byte offset */
		} else {
			d /= 2;	/* Word offset word pointer */
			printf("\tmov 3,%u\n", ac);
		}
		printf("\tlda 2,2,1\n");
		printf("\tadd 2,%u,skp\n", ac);
		printf("\t.word %d\n", d);
		return 1;
	case T_LOCAL:
		d += 2;		/* Byte offset for FP */
		if (is_bytepointer(n->type)) {
			/* Convert to byte pointer */
			printf("\tmovzl 3,%u\n", ac);
		} else {
			/* Work in words */
			printf("\tmov 3,%u\n", ac);
			d /= 2;
		}
		printf("\tlda 2,2,1\n");
		printf("\tadd 2,%u,skp\n", ac);
		printf("\t.word %d\n", d);
		return 1;
	case T_LREF:
		/* Will always be size 2 at this point */
		if (d >= -128 && d < 128) {
			printf("\tlda %u,%d,3\n", ac, d);
			return 1;
		}
		printf("\tmov 3,2\n");
		printf("\tlda %u,2,1\n", ac);
		printf("\tadd %u,2,skp\n", ac);
		printf("\t.word %d\n", d);
		printf("\tlda %u,0,2\n", ac);
		return 1;
	case T_CONSTANT:
		if (gen_constant(ac, v) == 0) {
			printf("\tjsr @__const%u,0\n", ac);
			printf("\t.word %u\n", v);
		}
		return 1;
	case T_NAME:
		printf("\tjsr @__const%u,0\n", ac);
		if (is_bytepointer(n->type))
			printf("\t.byteptr _%s+%u\n", namestr(n->snum), v);
		else
			printf("\t.word _%s+%u\n", namestr(n->snum), v);
		return 1;
	case T_LABEL:
		printf("\tjsr @__const%u,0\n", ac);
		if (is_bytepointer(n->type))
			printf("\t.byteptr T%u+%u\n", n->val2, v);
		else
			printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_NREF:/* Refs are always word at this point */
		printf("\tjsr @__iconst%u\n", ac);
		printf("\t.word _%s+%u\n", namestr(n->snum), v);
		return 1;
	case T_LBREF:
		printf("\tjsr @__iconst%u\n", ac);
		printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	}
	printf(";couldnt shortcut %x\n", n->op);
	return 0;
}

unsigned add_constant(uint16_t v)
{
	if (v == 0)
		return 1;
	if (v == 0xFFFF) {
		printf("\tneg 1,1\n");
		printf("\tcom 1,1\n");
		return 1;
	}
	if (v <= 3)
		repeated_op(v, "inc 1,1");
	else if (gen_constant(0, v)) {
		printf("\tadd 0,1\n");
	} else {
		printf("\tlda 0,2,1\n");
		printf("\tadd 0,1,skp\n");
		printf("\t.word %u\n", v & 0xFFFF);
	}
	return 1;
}

static void node_word(struct node *n)
{
	unsigned v = n->value;
	printf(";node word type %x\n", n->type);
	if (n->op == T_CONSTANT) {
		gen_wvalue(n->type, n->value);
		return;
	}	
	if (is_bytepointer(n->type))
		printf("\t.byteptr ");
	else
		printf("\t.word ");
	if (n->op == T_NAME)
		printf("_%s+%u\n", namestr(n->snum), v);
	else if (n->op == T_LABEL)
		printf("T%u+%u\n", n->val2, v);
	else
		error("nw");
}

/* Unless we can generate a value directly generate a helper call followed
   by the value for constants */
static unsigned const_condop(struct node *n, char *o, char *uo)
{
	printf(";n->type %x\n", n->type);
	if (get_size(n->type) == 4)
		return 0;

	/* FIXME: this test should just apply to gen_constant */
	if (n->op != T_CONSTANT)
		return 0;
	if (n->type & UNSIGNED)
		o = uo;
#if 0
	/* Not clear this is worth it */
	if (gen_constant(0, n->value))
		return 2;
#endif
	/* Constant didn't work, but we there are other wins */
	if (n->op != T_NAME && n->op != T_LABEL && n->op != T_CONSTANT)
		return 0;
	helper(n, o);
	node_word(n);
	return 1;
}

unsigned gen_fast_mul(unsigned v)
{
	if (v == 0) {
		gen_constant(1, 0);
		return 1;
	}
	if (v == 1)
		return 1;
	/* Inline trivial ones */
	if (v == 2) {
		printf("\tmovzl 1,1\n");
		return 1;
	}
	if (v == 4) {
		printf("\tmovzl 1,1\n");
		printf("\tmovzl 1,1\n");
		return 1;
	}
	/* TODO: helpers for other simpler ones */
	/* TODO: use MUL  if present */
	return 0;
}

unsigned gen_fast_div(struct node *n, unsigned v)
{
	if (v == 1)
		return 1;
	if (n->type & UNSIGNED) {
		if (v == 2) { 
			printf("\tmovzr 1,1\n");
			return 1;
		}
		if (v == 4) {
			printf("\tmovzr 1,1\n");
			printf("\tmovzr 1,1\n");
			return 1;
		}
		if (v == 8) {
			printf("\tmovzr 1,1\n");
			printf("\tmovzr 1,1\n");
			printf("\tmovzr 1,1\n");
			return 1;
		}
		if (v == 16) {
			printf("\tmovzr 1,1\n");
			printf("\tmovzr 1,1\n");
			printf("\tmovzr 1,1\n");
			printf("\tmovzr 1,1\n");
			return 1;
		}
	}
	return 0;
}

/*
 *	If possible turn this node into a direct access. We've already checked
 *	that the right hand side is suitable. If this returns 0 it will instead
 *	fall back to doing it stack based.
 */
unsigned gen_direct(struct node *n)
{
	struct node *r = n->right;
	unsigned s = get_size(n->type);
	unsigned nr = n->flags & NORETURN;
	unsigned v;

	switch(n->op) {
	/* Clean up is special and must be handled directly. It also has the
	   type of the function return so don't use that for the cleanup value
	   in n->right */
	case T_CLEANUP:
		v = r->value / 2;
		if (v >= 5) {
			printf("\tmfsp 1\n");
			printf("\tlda 0,2,1\n");
			printf("\tadd 0,1,skp\n");
			printf("\t.word %u\n", (-v) & 0xFFFF);
			printf("\tmtsp 1\n");
		} else
			repeated_op(v, "popa 0");
		sp -= v;
		return 1;
	case T_PLUS:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (add_constant(v))
				return 1;
		}
		if (load_ac(0, r)) {
			printf("\tadd 0,1\n");
			return 1;
		}
		break;
	case T_MINUS:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (add_constant(-v))
				return 1;
		}
		if (load_ac(0, r)) {
			printf("\tsub 0,1\n");
			return 1;
		}
		break;
	/* These ops we can do at the least */
	case T_AND:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (v == 0) {
				printf("\tsub 1,1\n");
				return 1;
			}
			if (v == 0xFFFF)
				return 1;
		}
		if (load_ac(0, r)) {
			printf("\tand 0,1\n");
			return 1;
		}
		break;
	case T_OR:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (v == 0)
				return 1;
			if (v == 0xFFFF) {
				printf("\tadc 1,1\n");	/* 0xFFFF */
				return 1;
			}
		}
		if (load_ac(0, r)) {
			printf("\tcom 0,0\n");
			printf("\tand 0,1\n");
			printf("\tadc 0,1\n");
			return 1;
		}
		break;
	case T_HAT:
		if (r->op == T_CONSTANT && s == 2) {
			v = r->value;
			if (v == 0)
				return 1;
			if (v == 0xFFFF) {
				printf("\tcom 1,1\n");
				return 1;
			}
		}
		if (load_ac(0, r)) {
			printf("\tmov 1,2\n");
			printf("\tandzl 0,2\n");
			printf("\tadd 0,1\n");
			printf("\tsub 2,1\n");
			return 1;
		}
		break;
	/* And some of the shift forms for constant */
	case T_LTLT:
	case T_GTGT:
		break;
	/* Plus some constant compares */
	case T_EQEQ:
		/* TODO: teach these all about the zero case shorter form */
		switch(const_condop(r, "condeq", "condeq")) {
		case 0:
			return 0;
		case 2:
			printf("\tsub 0,1,snr\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_BANGEQ:
		switch(const_condop(r, "condne", "condne")) {
		case 0:
			return 0;
		case 2:
			printf("\tsub 0,1,szr\n");
			printf("\tsubzl 1,1\n");
			/* if we skipped then AC1 is already zero */
		}
		n->flags |= ISBOOL;
		return 1;
	case T_LTEQ:
		switch(const_condop(r, "condlteq", "condltequ")) {
		case 0:
			return 0;
		case 2:
			printf("\tsubz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_GT:
		switch(const_condop(r, "condgt", "condgtu")) {
		case 0:
			return 0;
		case 2:
			printf("\tsubz# 1,0,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_GTEQ:
		switch(const_condop(r, "condgteq", "condgtequ")) {
		case 0:
			return 0;
		case 2:
			printf("\tadcz# 1,0,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	case T_LT:
		switch(const_condop(r, "condlt", "condltu")) {
		case 0:
			return 0;
		case 2:
			printf("\tadcz# 1,0,szc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
		}
		n->flags |= ISBOOL;
		return 1;
	/* Shrink some common eq ops */
	/* It would be good to have helpers for byte += as they can
	   be far more optimal than doing two sets of byte load/store
	   dances */
	case T_PLUSPLUS:
		if (!nr)
			return 0;
	case T_PLUSEQ:
		if (s != 1 && r->op == T_CONSTANT) {
			helper(n, "cpluseq");
			node_word(r);
			return 1;
		}
		return 0;
	case T_MINUSMINUS:
		if (!nr)
			return 0;
	case T_MINUSEQ:
		if (s != 1 && r->op == T_CONSTANT) {
			helper(n, "cpluseq");
			r->value = -r->value;
			node_word(r);
			return 1;
		}
		return 0;
	case T_SLASH:
		if (s == 2 && r->op == T_CONSTANT)
			return gen_fast_div(n, r->value);
		return 0;
	case T_STAR:
		if (s == 2 && r->op == T_CONSTANT)
			return gen_fast_mul(r->value);
		return 0;
	}
	return 0;
}

/*
 *	Allow the code generator to shortcut the generation of the argument
 *	of a single argument operator (for example to shortcut constant cases
 *	or simple name loads that can be done better directly)
 */
unsigned gen_uni_direct(struct node *n)
{
	return 0;
}

static void gen_isz(int d, unsigned s)
{
	if (s == 4) {
		printf("\tisz %d,3\n", d + 1);
		/* No op to reverse skip */
		printf("\tmov# 0,0,skp\n");
	}
	/* Run for single word or if low word
	   went to 0 - carry */
	printf("\tisz %d,3\n", d);
	/* This might skip */
	/* mffp 3 seems the fastest meh op */
	printf("\tmffp 3\n");
}

/*
 *	Allow the code generator to shortcut trees it knows
 */
unsigned gen_shortcut(struct node *n)
{
	unsigned s = get_size(n->type);
	struct node *l = n->left;
	struct node *r = n->right;
	unsigned nr = n->flags & NORETURN;

	/* Unreachable code we can shortcut into nothing ..bye.. */
	if (unreachable)
		return 1;
	/* The comma operator discards the result of the left side, then
	   evaluates the right. Avoid pushing/popping and generating stuff
	   that is surplus */
	if (n->op == T_COMMA) {
		l->flags |= NORETURN;
		codegen_lr(l);
		/* Parent determines child node requirements */
		r->flags |= nr;
		codegen_lr(r);
		return 1;
	}
	switch(n->op) {
	case T_PLUSPLUS:
		if (!nr)
			return 0;
	case T_PLUSEQ:	/* Very specific but common case */
		if ((l->op == T_LOCAL || l->op == T_ARGUMENT) && r->op == T_CONSTANT && r->value <= 2 && s == 2) {
			int d = l->value / 2;
			if (r->value == 0 && nr)
				return 1;
			if (l->op == T_ARGUMENT)
				d -= argbase;
			else
				d++;
			if (d < -128 || d >= 127 + s / 2)
				return 0;
			printf(";pluseq fast\n");
			if (r->value > 0) {
				gen_isz(d, s);
			}
			if (r->value == 2) {
				gen_isz(d, s);
			}
			if (!nr)
				printf("\tlda 1,%d,3\n", d);
			return 1;
		}
		break;
	}
	return 0;
}

unsigned gen_cast(struct node *n)
{
	unsigned lt = n->type;
	unsigned rt = n->right->type;
	unsigned ls;
	unsigned rs;

	/* Pointer conversions: byte->word or word<-byte. Useless ones
	   got eliminated earlier */
	if (PTR(rt) && PTR(lt)) {
		unsigned bt = BASE_TYPE(rt) & ~UNSIGNED;
		if (bt == VOID || bt == CCHAR)
			/* Convert byte pointer to word */
			printf("\tmovzr 1,1\n");
		else	/* Word pointer to byte */
			printf("\tmovzl 1,1\n");
		return 1;			
	}

	/* FIXME: check C spec - do casts of pointers to integer types
	   give the byteptr or the typed ptr ? */
	if (PTR(rt))
		rt = USHORT;
	if (PTR(lt))
		lt = USHORT;

	/* Floats and stuff handled by helper */
	if (!IS_INTARITH(lt) || !IS_INTARITH(rt))
		return 0;

	ls = get_size(lt);
	rs = get_size(rt);

	printf(";cast to %x(%u) from %x(%u)\n", lt,ls, rt,rs);
	/* Size shrink is not always free as we work in words */
	if (ls <= rs) {
		if (ls == 1 && rs > 1) {	/* Need to mask */
			printf("\tlda 0,N255,0\n");
			printf("\tand 0,1\n");
		}
		return 1;
	}
	/* Don't do the harder ones */
	if (rs == 1 && !(rt & UNSIGNED))
		return 0;
	/* All byte ops are word ops internally and the save and load
	   mask so conversion from char should be free. To char unusually
	   on the other hand is not - see above */
	if (ls == 2)
		return 1;
	printf("\tsub 0,0\n");
	if (!(rt & UNSIGNED)) {
		/* If top bit set then set ac0 to -1 */
		printf("\tmovl# 1,1,snc\n");
		printf("\tadc 0,0\n");
	}
	printf("\tsta 0,__hireg,0\n");
	return 1;
}

static struct node ntmp;

static unsigned do_eqop(struct node *n, unsigned op, unsigned cost)
{
	unsigned s = get_size(n->type);

	if (s != 1 && (cost > opt || optsize))
		return 0;

	/* At this point TOS is the pointer */

	if (s == 1)
		printf("\tjsr @__eqcget\n");
	else {
		printf("\tlda 2,0,3\n");
		printf("\tsta 1,__tmp,0\n");		/* Save working value */
		if (s == 2) {
			printf("\tlda 2,0,3\n");
			printf("\tlda 1,0,2\n");
			printf("\tpsha 1\n");
		} else if (s == 4) {
			printf("\tlda 2,0,3\n");
			printf("\tlda 0,0,2\n");
			printf("\tlda 1,0,2\n");
			printf("\tpsha 1\n");
			printf("\tpsha 0\n");
			printf("\tlda 1,__tmp,0\n");
		}
	}
	/* We now have things marshalled as we want them */

	memcpy(&ntmp, n, sizeof(ntmp));
	ntmp.op = op;
	ntmp.left = NULL;

	sp += get_stack_size(n->type) / 2;

	make_node(&ntmp);

	sp -= get_stack_size(n->type) / 2;

	/* Result is now in hireg:1 and arg is gone from stack */
	if (s == 2) {
		printf("\tpopa 2\n");
		printf("\tsta 1,0,2\n");
		return 1;
	}
	if (s == 4) {
		printf("\tpopa 2\n");
		printf("\tlda 0,__hireg,0\n");
		printf("\tsta 0,0,2\n");
		printf("\tsta 1,1,2\n");
		return 1;
	}
	printf("\tjsr @__assignc\n");
	return 1;
}

static void pop_signfix(unsigned type)
{
	printf("\tpopa 0\n");
	if (!(type & UNSIGNED)) {
		printf("\tmovl 0,0\n\tmovcr 0,0\n");
		printf("\tmovl 1,1\n\tmovcr 1,1\n");
	}
}

unsigned gen_node(struct node *n)
{
	struct node *r = n->right;
	unsigned v = n->value;
	unsigned s = get_size(n->type);
	int16_t d = v;
	unsigned nr = n->flags & NORETURN;

	/* Function call arguments are special - they are removed by the
	   act of call/return and reported via T_CLEANUP */
	if (n->left && n->op != T_ARGCOMMA && n->op != T_FUNCCALL && n->op != T_CALLNAME)
		sp -= get_stack_size(n->left->type) / 2;
	switch(n->op) {
	case T_CALLNAME:
		printf("\tjsr @1,1\n");
		printf("\t.word _%s+%u\n", namestr(n->snum), v);
		return 1;
	case T_CONSTANT:
		if (nr)
			return 1;
		/* We should do longs using make constant twice */
		if (s < 4 && gen_constant(1, v))
			return 1;
	case T_NAME:
	case T_LABEL:
		if (nr)
			return 1;
		v = n->value;
		if (s <= 2)
			printf("\tjsr @__const1,0\n");
		else
			printf("\tjsr @__const1l,0\n");
		node_word(n);
		return 1;
	case T_LBREF:
		if (nr)
			return 1;
	case T_NREF:
		/* Same logic but actual value */
		v = n->value;
		if (s == 2)
			printf("\tjsr @__iconst1,0\n");
		else
			printf("\tjsr @__iconst1l,0\n");
		if (n->op == T_NREF)
			printf("\t.word _%s+%u\n", namestr(n->snum), v);
		else
			printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_NSTORE:
	case T_LBSTORE:
		/* Same logic but store  */
		v = n->value;
		if (s == 2)
			printf("\tjsr @__sconst1,0\n");
		else
			printf("\tjsr @__sconst1l,0\n");
		if (n->op == T_NSTORE)
			printf("\t.word _%s+%u\n", namestr(n->snum), v);
		else
			printf("\t.word T%u+%u\n", n->val2, v);
		return 1;
	case T_ARGUMENT:
		if (nr)
			return 1;
		d = -(argbase + d);
		if (is_bytepointer(n->type)) {
			/* Byte pointer */
			printf("\tmovzl 3,1\n");
			d++;	/* In the low half of the argument word */
		} else {
			printf("\tmov 3,1\n");
			d /= 2;	/* Word machine */
		}
		if (d)
			add_constant(d);
		return 1;
	case T_LOCAL:
		if (nr)
			return 1;
		d += 2;		/* FP bias */
		if (is_bytepointer(n->type))
			printf("\tmovzl 3,1\n");
		else {
			printf("\tmov 3,1\n");
			d /= 2;	/* Word machine */
		}
		if (d)
			add_constant(d);
		/* TODO maybe optimize generally "add const to ac" for
		   the size tricks that work */
		return 1;
	case T_LREF:
		if (nr)
			return 1;
		/* An LREFPLUS rewrite might be useful to fold additions */
		/* TODO 4 is easy */
		if (d < 128 && d >= -128) {
			printf("\tlda 1,%d,3\n", (int)(int16_t)v);
			return 1;
		}
		/* Fix code dup with T_LOCAL */
		printf("\tmov 3,2\n");
		if (d) {
			printf("\tlda 0,2,1\n");
			printf("\tadd 0,2,skp\n");
			printf("\t.word %d\n", (int) d);
		}
		printf("\tlda 1,0,2\n");
		return 1;
	case T_LSTORE:
		if (d < 128 && d >= -128) {
			printf("\tsta 1,%d,3\n", (int)(int16_t)v);
			return 1;
		}
		/* Fix code dup with T_LOCAL */
		printf("\tmov 3,2\n");
		if (d) {
			printf("\tlda 0,2,1\n");
			printf("\tadd 0,2,skp\n");
			printf("\t.word %d\n", (int)d);
		}
		printf("\tsta 1,0,2\n");
		return 1;
	case T_DEREF:
		printf(";T_DEREF %u\n", s);
		if (nr)
			return 1;
		if (s == 1)
			return 0;
		printf("\tmov 1,2\n");
		if (s == 4) {
			printf("\tlda 1,2,2\n");
			printf("\tsta 1,__hireg,0\n");
		}
		printf("\tlda 1,0,2\n");
		return 1;
	case T_EQ:
		if (s == 1)	/* Byteops are hard */
			return 0;
		printf("\tpopa 2\n");
		printf("\tsta 1,0,2\n");
		if (s == 4) {
			printf("\tlda 0,__hireg,0\n");
			printf("\tsta 0,0,2\n");
		}
		return 1;
	case T_BOOL:
		/* Bool and conditionals the size is the size on the right */
		/* Already bool ? */
		if (r->flags & ISBOOL)
			return 1;
		s = get_size(r->type);
		if (s == 4) {
			printf("\tlda 0,__hireg,0\n");
			printf("\tmov# 1,1,snr\n");
			printf("\tmov# 0,0,szr\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			return 1;
		}
		if (s == 1) {
			printf("\tlda 0,__N255,0\n");
			printf("\tand 0,1\n");
		}
		printf("\tmov 1,1,szr\n");
		printf("\tsubzl 1,1\n");
		return 1;
	case T_BANG:
		/* TODO: could optimize bool not case to
		   com 1,1 inc 1,1 but we need to sort out the
		   conditional jump story first */
		s = get_size(r->type);
		if (s == 4) {
			printf("\tlda 0,__hireg,0\n");
			printf("\tmov# 1,1,snr\n");
			printf("\tmov# 0,0,szr\n");
			printf("\tsub 1,1,skp\n");
			printf("\tsubzl 1,1\n");
			return 1;
		}
		if (s == 1) {
			printf("\tlda 0,__N255,0\n");
			printf("\tand 0,1\n");
		}
		printf("\tmov 1,1,snr\n");
		printf("\tsubzl 1,1,skp\n");
		printf("\tsub 1,1\n");
		return 1;
	case T_PLUS:
		/* FIXME: broke for long. Should be
			add high, add low, if oveflow isz hireg,mffp 3 */
		printf("\tpopa 0\n");
		printf("\tadd 0,1\n");
		if (s == 4) {
			printf("\tpopa 2\n");
			printf("\tlda 0,__hireg,0\n");
			printf("\tadc 2,0\n");
			printf("\tsta 0,__hireg,0\n");
		}
		return 1;
	case T_MINUS:
		/* FIXME: broke for long. Should be
			add high, add low, if oveflow isz hireg,mffp 3 */
		printf("\tpopa 0\n");
		printf("\tsub 1,0\n");
		if (s == 4) {
			printf("\tpopa 2\n");
			printf("\tlda 1,__hireg,0\n");
			printf("\tsub 1,2\n");
			printf("\tsta 2,__hireg,0\n");
		}
		printf("\tmov 0,1\n");
		return 1;
	/* TODO T_SLASH, T_PERCENT, T_STAR via MUL/DIV if present */
	case T_STAR:
		if (0 && s == 2) {
			printf("\tpopa 2\n");
			printf("\tsub 0,0\n");
			printf("\tmul\n");
			return 1;
		}
		return 0;
	case T_SLASH:
		if (0 && s == 2 && (n->type & UNSIGNED)) {
			printf("\tmov 1,2\n");
			printf("\tsub 0,0\n");
			printf("\tpopa 1\n");
			printf("\tdiv\n");
			return 1;
		}
		return 0;
	case T_PERCENT:
		if (0 && s == 2 && (n->type & UNSIGNED)) {
			printf("\tmov 1,2\n");
			printf("\tsub 0,0\n");
			printf("\tpopa 1\n");
			printf("\tdiv\n");
			printf("\tmov 0,1\n");
			return 1;
		}
		return 0;
	case T_TILDE:
		printf("\tcom 1,1\n");
		if (s == 4 && !optsize) {
			printf("\tlda 0,__hireg,0\n");
			printf("\tcom 0,0\n");
			printf("\tsta 0,__hireg,0\n");
		}
		return 1;
	case T_NEGATE:
		if (s == 2) {
			printf("\tneg 1,1\n");
			return 1;
		}
		if (s == 4 && !optsize && n->type != FLOAT) {
			printf("\tneg 1,1,snr\n");
			printf("\tlda 0,__hireg,0\n");
			printf("\tneg 0,0,skp\n");
			printf("\tcom 0,0\n");
			printf("\tsta 0,__hireg,0\n");
			return 1;
		}
		return 0;
	case T_AND:
		printf("\tpopa 0\n");
		printf("\tand 0,1\n");
		if (s == 4) {
			printf("\tpopa 0\n");
			printf("\tlda 2,__hireg,0\n");
			printf("\tand 2,0\n");
			printf("\tsta 0,__hireg,0\n");
		}
		return 1;
	case T_OR:
		if (s == 4)
			return 0;
		printf("\tpopa 0\n");
		printf("\tcom 0,0\n");
		printf("\tand 0,1\n");
		printf("\tadc 0,1\n");
		return 1;
	case T_HAT:
		if (s == 4)
			return 0;
		printf("\tpopa 0\n");
		printf("\tmov 1,2\n");
		printf("\tandzl 0,2\n");
		printf("\tadd 0,1\n");
		printf("\tsub 2,1\n");
		return 1;
	case T_EQEQ:
		if (s == 4)
			return 0;
		printf("\tpopa 0\n");
		printf("\tsub 0,1,snr\n");
		printf("\tsubzl 1,1,skp\n");
		printf("\tsub 1,1\n");
		n->flags |= ISBOOL;
		return 1;
	case T_BANGEQ:
		if (s == 4)
			return 0;
		printf("\tpopa 0\n");
		printf("\tsub 0,1,szr\n");
		printf("\tsubzl 1,1\n");
		/* if we skipped then AC1 is already zero */
		n->flags |= ISBOOL;
		return 1;
	case T_CAST:
		return gen_cast(n);
	/* These ops we can do at the least for some forms */
	/* Apart from the popa keep them the same as the direct ones
	   as we will need to use peephole rules to fuse them with conditional
	   jumps, and also need CCONLY support in some cases to rewrite them
	   nicely */
	case T_LTEQ:
		s = get_size(r->type);
		if (s != 4) {
			pop_signfix(n->type);
			printf("\tsubz# 0,1,szc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	case T_GT:
		s = get_size(r->type);
		if (s != 4) {
			pop_signfix(n->type);
			printf("\tsubz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	case T_GTEQ:
		s = get_size(r->type);
		if (s != 4) {
			pop_signfix(n->type);
			printf("\tadcz# 0,1,snc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	case T_LT:
		s = get_size(r->type);
		if (s != 4) {
			pop_signfix(n->type);
			/* sets L if AC0 < AC1 */
			printf("\tadcz# 0,1,szc\n");
			printf("\tsubzl 1,1,skp\n");
			printf("\tsub 1,1\n");
			n->flags |= ISBOOL;
			return 1;
		}
		break;
	/* We do the eq ops as a load/op/store pattern in general */
	case T_PLUSEQ:
		printf(";pluseq\n");
		return do_eqop(n, T_PLUS, 2);
	case T_MINUSEQ:
		return do_eqop(n, T_MINUS, 2);
	case T_STAREQ:
		return do_eqop(n, T_STAR, 0);
	case T_SLASHEQ:
		return do_eqop(n, T_SLASH, 0);
	case T_PERCENTEQ:
		return do_eqop(n, T_PERCENT, 0);
	case T_ANDEQ:
		return do_eqop(n, T_AND, 2);
	case T_OREQ:
		return do_eqop(n, T_OR, 2);
	case T_HATEQ:
		return do_eqop(n, T_HAT, 2);
	case T_SHLEQ:
		return do_eqop(n, T_LTLT, 2);
	case T_SHREQ:
		return do_eqop(n, T_GTGT, 2);
	/* These are harder */
	case T_PLUSPLUS:
		if (nr)
			return do_eqop(n, T_PLUS, 2);
		return 0;
	case T_MINUSMINUS:
		if (nr)
			return do_eqop(n, T_MINUS, 2);
		return 0;
	}
	return 0;
}
