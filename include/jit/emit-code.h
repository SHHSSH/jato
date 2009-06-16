#ifndef JATO_EMIT_CODE_H
#define JATO_EMIT_CODE_H

struct compilation_unit;
struct jit_trampoline;
struct basic_block;
struct buffer;
struct object;

enum emitter_type {
	NO_OPERANDS = 1,
	SINGLE_OPERAND,
	TWO_OPERANDS,
	BRANCH,
};

struct emitter {
	void *emit_fn;
	enum emitter_type type;
};

extern struct emitter emitters[];

#define DECL_EMITTER(_insn_type, _fn, _emitter_type) \
	[_insn_type] = { .emit_fn = _fn, .type = _emitter_type }

void emit_prolog(struct buffer *, unsigned long);
void emit_epilog(struct buffer *);
void emit_trampoline(struct compilation_unit *, void *, struct jit_trampoline *);
void emit_unwind(struct buffer *);
void emit_lock(struct buffer *, struct object *);
void emit_lock_this(struct buffer *);
void emit_unlock(struct buffer *, struct object *);
void emit_unlock_this(struct buffer *);
void backpatch_branch_target(struct buffer *, struct insn *, unsigned long);

#endif /* JATO_EMIT_CODE_H */
