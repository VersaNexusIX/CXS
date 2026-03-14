/*
 * transform.c  —  CXS Polymorphic Transform Engine  v4.0
 * ============================================================
 * All 15 obfuscation passes, each independently callable:
 *
 *  T1   cxs_transform_arithmetic()
 *  T2   cxs_inject_junk()
 *  T3   cxs_reorder_blocks()
 *  T4   cxs_insert_opaque_predicates()
 *  T5   cxs_rename_registers()
 *  T6   cxs_insert_overlap_bytes()
 *  T7   cxs_flatten_control_flow()
 *  T8   cxs_encode_constants()
 *  T9   cxs_indirect_control_flow()
 *  T10  cxs_insert_dead_code()
 *  T11  cxs_substitute_instructions()
 *  T12  cxs_obfuscate_data_flow()
 *  T13  cxs_mangle_stack_frame()
 *  T14  cxs_insert_decrypt_stub()
 *  T15  cxs_insert_antiana_markers()
 *
 *  cxs_run_pipeline()  —  full 15-pass pipeline in recommended order
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cxs.h"

/* ============================================================
 * Internal emit helpers  (shared across all passes)
 * ============================================================ */

/* Build operand values */
static operand_t op_reg(reg_id_t r) {
    operand_t o={0}; o.type=OP_REG; o.reg=r; return o;
}
static operand_t op_imm(int64_t v) {
    operand_t o={0}; o.type=OP_IMM; o.imm=v; return o;
}
static operand_t op_lbl(const char *n) {
    operand_t o={0}; o.type=OP_LABEL;
    snprintf(o.label,CXS_LABEL_LEN,"%s",n); return o;
}

/* Insert one instruction at index `pos`, shifting the rest right.
 * Returns pointer to the newly created slot. */
static insn_t *insn_insert(cxs_engine_t *e, int pos, const insn_t *src) {
    if (e->num_insns >= CXS_MAX_INSN-1) return NULL;
    memmove(&e->insns[pos+1], &e->insns[pos],
            (size_t)(e->num_insns-pos)*sizeof(insn_t));
    e->num_insns++;
    e->insns[pos] = *src;
    return &e->insns[pos];
}

/* Append an instruction at the end of the stream */
static insn_t *insn_append(cxs_engine_t *e __attribute__((unused)), insn_type_t type __attribute__((unused))) __attribute__((unused));
static insn_t *insn_append(cxs_engine_t *e, insn_type_t type) {
    if (e->num_insns >= CXS_MAX_INSN) return NULL;
    insn_t *i = &e->insns[e->num_insns++];
    memset(i, 0, sizeof(*i));
    i->type     = type;
    i->block_id = -1;
    return i;
}

/* ============================================================
 * T1 — Arithmetic Substitution
 *
 * Replaces arithmetic opcodes with semantically equivalent forms:
 *   ADD rax, K      →  SUB rax, -K          (negate immediate)
 *   SUB rax, K      →  ADD rax, -K
 *   INC rax         →  ADD rax, 1
 *   DEC rax         →  SUB rax, 1
 *   IMUL rax, 2     →  ADD rax, rax          (shift-equivalent)
 * ============================================================ */
void cxs_transform_arithmetic(cxs_engine_t *e) {
    int count = 0;
    for (int i = 0; i < e->num_insns; i++) {
        insn_t *ins = &e->insns[i];
        if (ins->is_junk || ins->is_opaque) continue;

        if ((ins->type == INSN_ADD || ins->type == INSN_SUB) &&
             ins->num_ops == 2 && ins->ops[1].type == OP_IMM) {
            ins->ops[1].imm = -ins->ops[1].imm;
            ins->type = (ins->type == INSN_ADD) ? INSN_SUB : INSN_ADD;
            count++;

        } else if (ins->type == INSN_INC && ins->num_ops == 1) {
            ins->type      = INSN_ADD;
            ins->ops[1]    = op_imm(1);
            ins->num_ops   = 2;
            count++;

        } else if (ins->type == INSN_DEC && ins->num_ops == 1) {
            ins->type      = INSN_SUB;
            ins->ops[1]    = op_imm(1);
            ins->num_ops   = 2;
            count++;

        } else if (ins->type == INSN_IMUL && ins->num_ops == 2 &&
                   ins->ops[1].type == OP_IMM && ins->ops[1].imm == 2) {
            /* IMUL rax, 2  →  ADD rax, rax */
            ins->type    = INSN_ADD;
            ins->ops[1]  = op_reg(ins->ops[0].reg);
            count++;
        }
    }
    e->stats.arith_subs   += count;
    e->stats.total_transforms++;
    printf("    [T1]  %d arithmetic substitutions applied.\n", count);
}

/* ============================================================
 * T2 — Junk / NOP Sled Injection
 *
 * Inserts dead instructions after real instructions.
 * Variants: NOP, XOR r,r, AND r,-1, OR r,0, MOV r,0, CMP r,r
 * The VM skips all instructions with is_junk=1.
 * ============================================================ */
void cxs_inject_junk(cxs_engine_t *e) {
    /* Scratch registers safe for junk (never rax/rsp/rbp) */
    reg_id_t pool[] = { REG_R10, REG_R11, REG_R12 };
    const int plen  = 3;
    int count = 0;

    for (int i = e->num_insns-1; i >= 0; i--) {
        insn_t *ins = &e->insns[i];
        if (ins->is_junk || ins->is_opaque || ins->type == INSN_LABEL) continue;
        if (e->num_insns >= CXS_MAX_INSN - CXS_MAX_JUNK) break;

        /* Inject 1–3 junk insns after position i */
        int n = cxs_rand_range(e, 1, 3);
        for (int k = 0; k < n && e->num_insns < CXS_MAX_INSN-1; k++) {
            insn_t junk; memset(&junk, 0, sizeof(junk));
            junk.is_junk   = 1;
            junk.block_id  = ins->block_id;

            reg_id_t r = pool[cxs_rand_range(e, 0, plen-1)];
            switch (cxs_rand_range(e, 0, 5)) {
                case 0: junk.type=INSN_NOP;  junk.num_ops=0; break;
                case 1: junk.type=INSN_XOR;  junk.ops[0]=op_reg(r);junk.ops[1]=op_reg(r);junk.num_ops=2; break;
                case 2: junk.type=INSN_AND;  junk.ops[0]=op_reg(r);junk.ops[1]=op_imm(-1);junk.num_ops=2; break;
                case 3: junk.type=INSN_OR;   junk.ops[0]=op_reg(r);junk.ops[1]=op_imm(0); junk.num_ops=2; break;
                case 4: junk.type=INSN_MOV;  junk.ops[0]=op_reg(r);junk.ops[1]=op_imm(0); junk.num_ops=2; break;
                case 5: junk.type=INSN_CMP;  junk.ops[0]=op_reg(r);junk.ops[1]=op_reg(r); junk.num_ops=2; break;
            }
            insn_insert(e, i+1+k, &junk);
            count++;
        }
    }
    e->stats.junk_injected += count;
    e->stats.total_transforms++;
    printf("    [T2]  %d junk instructions injected.\n", count);
}

/* ============================================================
 * T3 — Block Reordering + JMP Glue
 *
 * Shuffles blocks 1..N-1 (entry block 0 stays at the front).
 * Inserts unconditional JMP glue between non-consecutive blocks
 * so execution order is preserved.
 * ============================================================ */
void cxs_reorder_blocks(cxs_engine_t *e) {
    if (e->num_blocks < 2) {
        printf("    [T3]  Not enough blocks to reorder (%d).\n", e->num_blocks);
        return;
    }

    /* Fisher-Yates shuffle of blocks 1..N-1 */
    int *order = e->exec_order;
    for (int i = 0; i < e->num_blocks; i++) order[i] = i;
    for (int i = e->num_blocks-1; i > 1; i--) {
        int j = cxs_rand_range(e, 1, i);
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }

    /* Rebuild instruction stream in new order, inserting glue JMPs */
    insn_t  tmp[CXS_MAX_INSN];
    int     tmp_n = 0;

    for (int oi = 0; oi < e->num_blocks; oi++) {
        int b = order[oi];
        block_t *blk = &e->blocks[b];

        /* Use block_id field (not stale start/end) to find all block instructions */
        for (int i = 0; i < e->num_insns; i++) {
            if (e->insns[i].block_id != b) continue;
            if (tmp_n < CXS_MAX_INSN) tmp[tmp_n++] = e->insns[i];
        }

        /* If this block's natural successor is not the next in our order,
         * insert a JMP to the correct label */
        int want_next = (oi+1 < e->num_blocks) ? order[oi+1] : -1;
        if (blk->next_id >= 0 && blk->next_id != want_next) {
            /* Check if last insn is already a JMP/RET */
            int last_is_jump = 0;
            if (tmp_n > 0) {
                insn_type_t lt = tmp[tmp_n-1].type;
                last_is_jump = (lt==INSN_JMP||lt==INSN_RET||lt==INSN_IJMP);
            }
            if (!last_is_jump && tmp_n < CXS_MAX_INSN) {
                insn_t g; memset(&g, 0, sizeof(g));
                g.type     = INSN_JMP;
                g.block_id = b;
                g.ops[0]   = op_lbl(e->blocks[blk->next_id].label);
                g.num_ops  = 1;
                tmp[tmp_n++] = g;
            }
        }
    }

    memcpy(e->insns, tmp, (size_t)tmp_n * sizeof(insn_t));
    e->num_insns = tmp_n;
    e->num_exec_order = e->num_blocks;

    /* Report new order */
    printf("    [T3]  Block order: ");
    for (int i = 0; i < e->num_blocks; i++)
        printf("%s%s", e->blocks[order[i]].label, (i<e->num_blocks-1)?"  →  ":"");
    printf("\n");

    e->stats.blocks_reordered += e->num_blocks;
    e->stats.total_transforms++;
}

/* ============================================================
 * T4 — Opaque Predicates
 *
 * Inserts always-false branch clusters between real instructions.
 * Five mathematically proven never-taken patterns:
 *
 *   0: TEST r,0  → ZF=1 → JNE dead   (x&0 is always 0)
 *   1: AND r,-1  → ZF=? → JE dead     (x&-1 = x, nonzero if r≠0)
 *   2: CMP r,r   → ZF=1 → JNE dead   (x-x = 0 always)
 *   3: OR r,0;CMP r,r → JNE dead
 *   4: XOR r,r   → ZF=1 → JNE dead
 *
 * The dead block contains garbage instructions the VM never reaches.
 * ============================================================ */
void cxs_insert_opaque_predicates(cxs_engine_t *e) {
    reg_id_t pool[] = { REG_R10, REG_R11, REG_R12 };
    int count = 0;

    for (int i = e->num_insns-1; i >= 0; i--) {
        insn_t *ins = &e->insns[i];
        if (ins->is_junk || ins->is_opaque || ins->is_overlap) continue;
        if (ins->type == INSN_LABEL || ins->type == INSN_RET)  continue;
        if (e->num_insns >= CXS_MAX_INSN - 10) break;
        if (cxs_rand_range(e, 0, 3) != 0) continue;  /* ~25% chance */

        reg_id_t r    = pool[cxs_rand_range(e, 0, 2)];
        int      kind = cxs_rand_range(e, 0, 4);
        int      ser  = e->opaque_serial++;

        char dead_lbl[CXS_LABEL_LEN], skip_lbl[CXS_LABEL_LEN];
        snprintf(dead_lbl, CXS_LABEL_LEN, ".op_dead_%d", ser);
        snprintf(skip_lbl, CXS_LABEL_LEN, ".op_skip_%d", ser);

        /* Build the opaque cluster in a temp buffer */
        insn_t cluster[10]; int cn = 0;

/* Helper: initialize next cluster slot */
#define CL_INIT(itype) do { \
    memset(&cluster[cn],0,sizeof(cluster[cn])); \
    cluster[cn].type=itype; cluster[cn].is_opaque=1; \
    cluster[cn].block_id=ins->block_id; \
} while(0)
#define CL_2OP(a,b) do { cluster[cn].ops[0]=(a);cluster[cn].ops[1]=(b);cluster[cn].num_ops=2; } while(0)
#define CL_1OP(a)   do { cluster[cn].ops[0]=(a); cluster[cn].num_ops=1; } while(0)
#define CL_PUSH     cn++

        switch (kind) {
            case 0:
                CL_INIT(INSN_TEST);  CL_2OP(op_reg(r),op_imm(0));           CL_PUSH;
                CL_INIT(INSN_JNE);   CL_1OP(op_lbl(dead_lbl));               CL_PUSH;
                break;
            case 1:
                CL_INIT(INSN_AND);   CL_2OP(op_reg(r),op_imm(-1));           CL_PUSH;
                CL_INIT(INSN_CMP);   CL_2OP(op_reg(r),op_reg(r));            CL_PUSH;
                CL_INIT(INSN_JNE);   CL_1OP(op_lbl(dead_lbl));               CL_PUSH;
                break;
            case 2:
                CL_INIT(INSN_CMP);   CL_2OP(op_reg(r),op_reg(r));            CL_PUSH;
                CL_INIT(INSN_JNE);   CL_1OP(op_lbl(dead_lbl));               CL_PUSH;
                break;
            case 3:
                CL_INIT(INSN_OR);    CL_2OP(op_reg(r),op_imm(0));            CL_PUSH;
                CL_INIT(INSN_CMP);   CL_2OP(op_reg(r),op_reg(r));            CL_PUSH;
                CL_INIT(INSN_JNE);   CL_1OP(op_lbl(dead_lbl));               CL_PUSH;
                break;
            case 4:
                CL_INIT(INSN_XOR);   CL_2OP(op_reg(r),op_reg(r));            CL_PUSH;
                CL_INIT(INSN_JNE);   CL_1OP(op_lbl(dead_lbl));               CL_PUSH;
                break;
        }

        /* Skip-over JMP (real flow) */
        CL_INIT(INSN_JMP);   CL_1OP(op_lbl(skip_lbl));  CL_PUSH;

        /* Dead block label */
        CL_INIT(INSN_LABEL); cluster[cn].has_label=1;
        snprintf(cluster[cn].label, CXS_LABEL_LEN, "%s", dead_lbl);  CL_PUSH;
        /* Dead body */
        CL_INIT(INSN_XOR);   CL_2OP(op_reg(r),op_imm(0xDEAD));       CL_PUSH;
        CL_INIT(INSN_SUB);   CL_2OP(op_reg(REG_RAX),op_reg(REG_RAX)); CL_PUSH;

        /* Skip label */
        CL_INIT(INSN_LABEL); cluster[cn].has_label=1;
        snprintf(cluster[cn].label, CXS_LABEL_LEN, "%s", skip_lbl);  CL_PUSH;

#undef CL_INIT
#undef CL_2OP
#undef CL_1OP
#undef CL_PUSH

        /* Insert cluster after current instruction */
        for (int k = cn-1; k >= 0; k--)
            insn_insert(e, i+1, &cluster[k]);

        count++;
        printf("    [T4]  Opaque predicate kind=%d inserted at [%3d]\n", kind, i);
    }

    e->stats.opaque_inserted += count;
    e->stats.total_transforms++;
    printf("    [T4]  %d opaque predicate clusters inserted.\n", count);
}

/* ============================================================
 * T5 — Register Renaming
 *
 * Applies a cycle-aware permutation to scratch registers R10–R12.
 * Locked registers (rax/rsp/rbp) and dedicated T7/T8/T9 registers
 * (r13/r14/r15) are never renamed.
 * ============================================================ */
void cxs_rename_registers(cxs_engine_t *e) {
    reg_id_t pool[] = { REG_R10, REG_R11, REG_R12 };
    const int plen  = 3;

    /* Fisher-Yates shuffle of the pool */
    reg_id_t shuffled[3];
    memcpy(shuffled, pool, sizeof(pool));
    for (int i = plen-1; i > 0; i--) {
        int j = cxs_rand_range(e, 0, i);
        reg_id_t t = shuffled[i]; shuffled[i]=shuffled[j]; shuffled[j]=t;
    }

    /* Build rename map: pool[i] → shuffled[i] */
    reg_id_t rename[REG_COUNT];
    for (int i = 0; i < REG_COUNT; i++) rename[i] = (reg_id_t)i;
    for (int i = 0; i < plen; i++)
        rename[pool[i]] = shuffled[i];

    /* Apply map to every non-locked operand */
    int count = 0;
    for (int i = 0; i < e->num_insns; i++) {
        insn_t *ins = &e->insns[i];
        for (int j = 0; j < ins->num_ops; j++) {
            operand_t *op = &ins->ops[j];
            if (op->type == OP_REG && !e->rmap.locked[op->reg]) {
                op->reg = rename[op->reg];
                count++;
            }
            if (op->type == OP_MEM && !e->rmap.locked[op->mem.base]) {
                op->mem.base = rename[op->mem.base];
                count++;
            }
        }
    }

    printf("    [T5]  Register swap: r10→%s  r11→%s  r12→%s\n",
           cxs_reg_name(shuffled[0]),
           cxs_reg_name(shuffled[1]),
           cxs_reg_name(shuffled[2]));
    printf("    [T5]  %d register operands renamed.\n", count);

    e->stats.regs_renamed     += count;
    e->stats.total_transforms++;
}

/* ============================================================
 * T6 — Instruction Overlap Bytes  (polyglot ASM patterns)
 *
 * Reads byte sequences from real assembled functions in the arch
 * ASM file (cxs_asm_*.S) using linker-computed symbol sizes.
 * Each pattern is a JMP-over sequence where the skipped bytes
 * look like valid (but different) instructions to a disassembler
 * that doesn't follow control flow.
 * ============================================================ */

#define N_OV_PATS     5
#define OV_PAT_MAXBYTES 16

typedef struct {
    uint8_t bytes[OV_PAT_MAXBYTES];
    int     len;
    const char *name;
} ov_pat_t;

static ov_pat_t OV_PATS[N_OV_PATS];
static int      ov_pats_ready = 0;

static void cxs_t6_init(void) {
    if (ov_pats_ready) return;

    const char *names[N_OV_PATS] = {
        "JMP+3/shadow-CALL",
        "JMP+4/ENDBR64-ghost",
        "JMP+2/JMP-RAX-shadow",
        "JMP+3/multi-prefix-NOP",
        "JMP+5/JMP32-DEADBEEF"
    };

#if defined(CXS_ARCH_X86_64) || defined(CXS_ARCH_ARM64)
    uint8_t *starts[N_OV_PATS] = {
        cxs_ovpat_p0, cxs_ovpat_p1, cxs_ovpat_p2,
        cxs_ovpat_p3, cxs_ovpat_p4
    };
    uint8_t *ends[N_OV_PATS] = {
        cxs_ovpat_p0_end, cxs_ovpat_p1_end, cxs_ovpat_p2_end,
        cxs_ovpat_p3_end, cxs_ovpat_p4_end
    };
    for (int i = 0; i < N_OV_PATS; i++) {
        int len = (int)(ends[i] - starts[i]);
        if (len <= 0 || len > OV_PAT_MAXBYTES) {
            OV_PATS[i].len = 0; OV_PATS[i].name = "(invalid)"; continue;
        }
        memcpy(OV_PATS[i].bytes, starts[i], (size_t)len);
        OV_PATS[i].len  = len;
        OV_PATS[i].name = names[i];
    }
#else
    for (int i = 0; i < N_OV_PATS; i++) {
        OV_PATS[i].len = 0; OV_PATS[i].name = "(no-asm-fallback)";
    }
#endif
    ov_pats_ready = 1;
}

void cxs_insert_overlap_bytes(cxs_engine_t *e) {
    cxs_t6_init();
    int inserted = 0;

    for (int i = e->num_insns-1; i >= 0; i--) {
        insn_t *ins = &e->insns[i];
        if (ins->is_junk || ins->is_opaque || ins->is_overlap) continue;
        if (ins->type == INSN_LABEL || ins->type == INSN_RET)  continue;
        if (e->num_insns >= CXS_MAX_INSN-1) break;
        if (cxs_rand_range(e, 0, 5) != 0) continue;  /* ~17% chance */

        /* Pick a non-empty pattern */
        int pat = cxs_rand_range(e, 0, N_OV_PATS-1);
        if (OV_PATS[pat].len <= 0) continue;

        insn_t ov; memset(&ov, 0, sizeof(ov));
        ov.type        = INSN_OVERLAP;
        ov.is_overlap  = 1;
        ov.block_id    = ins->block_id;
        ov.overlap_len = OV_PATS[pat].len;
        memcpy(ov.overlap_bytes, OV_PATS[pat].bytes, (size_t)ov.overlap_len);

        insn_insert(e, i, &ov);
        printf("    [T6]  +%-30s  bytes:", OV_PATS[pat].name);
        for (int b = 0; b < ov.overlap_len; b++) printf(" %02X", ov.overlap_bytes[b]);
        printf("\n");
        inserted++;
    }

    e->stats.overlap_inserted += inserted;
    e->stats.total_transforms++;
    printf("    [T6]  %d overlap sequences inserted.\n", inserted);
}

/* ============================================================
 * T7 — Control Flow Flattening  (CFF)
 *
 * Converts the block-structured IR into a single switch-dispatch loop:
 *
 *   MOV __state, <entry_state>
 * .cff_loop:
 *   CMP __state, 0  →  JE .cff_b<i>
 *   CMP __state, 1  →  JE .cff_b<j>
 *   ...
 *   JMP .cff_done
 *
 * .cff_b<i>:
 *   <original block i instructions, with JMPs → STATE_MOV + JMP .cff_loop>
 *
 * .cff_done:
 *   RET
 *
 * State IDs are Fisher-Yates shuffled so numeric order ≠ execution order.
 * ============================================================ */

/* Helpers reused by T7 */
static insn_t mk_flat(insn_type_t itype, int blk_id) {
    insn_t i; memset(&i,0,sizeof(i));
    i.type=itype; i.is_flat=1; i.block_id=blk_id;
    return i;
}

void cxs_flatten_control_flow(cxs_engine_t *e) {
    if (e->flat_done) {
        printf("    [T7]  Already flattened — skipping.\n"); return;
    }
    if (e->num_blocks < 2) {
        printf("    [T7]  Not enough blocks (%d) — skipping.\n", e->num_blocks); return;
    }

    /* Assign randomised state IDs */
    int order[CXS_MAX_BLOCKS];
    for (int i=0;i<e->num_blocks;i++) order[i]=i;
    for (int i=e->num_blocks-1;i>0;i--) {
        int j=cxs_rand_range(e,0,i);
        int t=order[i];order[i]=order[j];order[j]=t;
    }
    for (int i=0;i<e->num_blocks;i++) e->blocks[order[i]].state_id=i;
    int entry_state=e->blocks[0].state_id;

    /* Save original instruction stream */
    insn_t orig[CXS_MAX_INSN];
    int    orig_n=e->num_insns;
    memcpy(orig,e->insns,orig_n*sizeof(insn_t));
    e->num_insns=0;

#define FLAT_EMIT(itype, blk) \
    (&e->insns[e->num_insns] != NULL && e->num_insns < CXS_MAX_INSN-1 ? \
     (e->insns[e->num_insns]=mk_flat((itype),(blk)), &e->insns[e->num_insns++]) : NULL)

    /* Prologue: set initial state */
    { insn_t *s=FLAT_EMIT(INSN_STATE_MOV,-1);
      if(s){s->ops[0]=op_reg(REG_STATE);s->ops[1]=op_imm(entry_state);s->num_ops=2;} }

    /* Dispatcher label */
    { insn_t *l=FLAT_EMIT(INSN_LABEL,-1);
      if(l){l->has_label=1;snprintf(l->label,CXS_LABEL_LEN,".cff_loop");} }

    /* CMP + JE chain for each block */
    for (int b=0;b<e->num_blocks;b++) {
        char blbl[CXS_LABEL_LEN]; snprintf(blbl,CXS_LABEL_LEN,".cff_b%d",b);
        insn_t *cmp=FLAT_EMIT(INSN_STATE_CMP,-1);
        if(cmp){cmp->ops[0]=op_reg(REG_STATE);cmp->ops[1]=op_imm(e->blocks[b].state_id);cmp->num_ops=2;}
        insn_t *je=FLAT_EMIT(INSN_JE,-1);
        if(je){je->ops[0]=op_lbl(blbl);je->num_ops=1;}
    }

    /* Fallthrough → done (should never reach here) */
    { insn_t *j=FLAT_EMIT(INSN_JMP,-1);
      if(j){j->ops[0]=op_lbl(".cff_done");j->num_ops=1;} }

    /* Per-block bodies */
    for (int b=0;b<e->num_blocks;b++) {
        char blbl[CXS_LABEL_LEN]; snprintf(blbl,CXS_LABEL_LEN,".cff_b%d",b);
        insn_t *lbl=FLAT_EMIT(INSN_LABEL,b);
        if(lbl){lbl->has_label=1;snprintf(lbl->label,CXS_LABEL_LEN,"%s",blbl);}

        /* Copy instructions belonging to this block (by block_id) */
        for (int i=0;i<orig_n && e->num_insns<CXS_MAX_INSN-5;i++) {
            insn_t *src=&orig[i];
            if (src->block_id!=b) continue;
            /* Skip bare LABEL pseudo-insns */
            if (src->type==INSN_LABEL && !src->is_junk) continue;

            /* Replace JMPs with state update + loop-back */
            if ((src->type==INSN_JMP||src->type==INSN_JE||src->type==INSN_JNE)
                 && src->ops[0].type==OP_LABEL) {
                int tgt=-1;
                for (int tb=0;tb<e->num_blocks;tb++)
                    if(strcmp(e->blocks[tb].label,src->ops[0].label)==0){tgt=tb;break;}
                if (tgt>=0) {
                    insn_t *sm=FLAT_EMIT(INSN_STATE_MOV,b);
                    if(sm){sm->ops[0]=op_reg(REG_STATE);sm->ops[1]=op_imm(e->blocks[tgt].state_id);sm->num_ops=2;}
                    insn_t *jl=FLAT_EMIT(INSN_JMP,b);
                    if(jl){jl->ops[0]=op_lbl(".cff_loop");jl->num_ops=1;}
                    continue;
                }
            }
            /* RET → jump to epilogue */
            if (src->type==INSN_RET) {
                insn_t *jd=FLAT_EMIT(INSN_JMP,b);
                if(jd){jd->ops[0]=op_lbl(".cff_done");jd->num_ops=1;}
                continue;
            }

            if (e->num_insns < CXS_MAX_INSN-1) {
                e->insns[e->num_insns]=*src;
                e->insns[e->num_insns].is_flat=1;
                e->num_insns++;
            }
        }

        /* Fallthrough state update if block has a natural successor */
        if (e->blocks[b].next_id>=0 && e->blocks[b].next_id<e->num_blocks) {
            int ns=e->blocks[e->blocks[b].next_id].state_id;
            insn_t *sm=FLAT_EMIT(INSN_STATE_MOV,b);
            if(sm){sm->ops[0]=op_reg(REG_STATE);sm->ops[1]=op_imm(ns);sm->num_ops=2;}
            insn_t *jl=FLAT_EMIT(INSN_JMP,b);
            if(jl){jl->ops[0]=op_lbl(".cff_loop");jl->num_ops=1;}
        }
    }

    /* Epilogue */
    { insn_t *l=FLAT_EMIT(INSN_LABEL,-1);
      if(l){l->has_label=1;snprintf(l->label,CXS_LABEL_LEN,".cff_done");} }
    { insn_t *r=FLAT_EMIT(INSN_RET,-1); (void)r; }

#undef FLAT_EMIT

    e->flat_done=1;
    e->stats.blocks_flattened=e->num_blocks;
    e->stats.total_transforms++;
    printf("    [T7]  %d blocks flattened into CFF dispatch loop.\n",e->num_blocks);
    printf("    [T7]  State register: %s   Entry state: %d\n",
           cxs_reg_name(REG_STATE), entry_state);
    for (int b=0;b<e->num_blocks;b++)
        printf("    [T7]    block %-14s  →  state %d\n",
               e->blocks[b].label, e->blocks[b].state_id);
}

/* ============================================================
 * T8 — Constant Encoding
 *
 * Replaces plain immediate literals with a 2-instruction decode:
 *   Original:  ADD rax, 5
 *   Encoded:   MOV r15, (5 ^ KEY)   ; r15 = encoded value
 *              XOR r15, KEY          ; r15 = 5  (decoded)
 *              ADD rax, r15          ; same semantics
 *
 * Three schemes (chosen per-constant at random):
 *   XOR — val_stored = val ^ key    decode: stored ^ key
 *   ADD — val_stored = val + key    decode: stored - key
 *   ROL — stored = ROL(val, k)      decode: ROR(stored, k)
 *         (VM stores val directly for correctness)
 * ============================================================ */

static uint64_t rol64(uint64_t v, int n) {
    n &= 63; return n ? ((v<<n)|(v>>(64-n))) : v;
}
static uint64_t ror64(uint64_t v, int n) __attribute__((unused));
static uint64_t ror64(uint64_t v, int n) {
    n &= 63; return n ? ((v>>n)|(v<<(64-n))) : v;
}

void cxs_encode_constants(cxs_engine_t *e) {
    int encoded=0;
    for (int i=e->num_insns-1;i>=0;i--) {
        insn_t *ins=&e->insns[i];
        if (ins->is_junk||ins->is_opaque||ins->is_overlap||ins->is_dead) continue;
        if (ins->type==INSN_LABEL||ins->type==INSN_RET) continue;
        if (ins->type==INSN_JMP||ins->type==INSN_JE||ins->type==INSN_JNE) continue;
        if (ins->is_encoded) continue;
        if (e->num_insns>=CXS_MAX_INSN-3) break;

        int imm_idx=-1;
        for (int j=0;j<ins->num_ops;j++)
            if(ins->ops[j].type==OP_IMM){imm_idx=j;break;}
        if (imm_idx<0) continue;
        if (cxs_rand_range(e,0,9)>=9) continue;  /* ~90% chance */

        int64_t val=ins->ops[imm_idx].imm;
        int     kind=cxs_rand_range(e,0,2);
        int64_t key=(int64_t)((uint64_t)cxs_rand_range(e,1,0x7FFF)|
                    ((uint64_t)cxs_rand_range(e,1,0x7FFF)<<16));
        int64_t stored;
        const char *scheme;

        if (kind==0) {
            stored=(int64_t)((uint64_t)val^(uint64_t)key); scheme="XOR";
        } else if (kind==1) {
            stored=val+key; scheme="ADD";
        } else {
            int rot=cxs_rand_range(e,1,31);
            stored=(int64_t)rol64((uint64_t)val,rot);
            key=rot; scheme="ROL";
        }

        printf("    [T8]  [%3d] imm %-8"PRId64" via %s  "
               "(stored=0x%"PRIx64" key=0x%"PRIx64")\n",
               i, val, scheme, (uint64_t)stored, (uint64_t)key);

        /* MOV r15, stored_value */
        insn_t ma; memset(&ma,0,sizeof(ma));
        ma.type=INSN_MOV; ma.is_encoded=1; ma.block_id=ins->block_id;
        ma.ops[0]=op_reg(REG_CTMP); ma.ops[1]=op_imm(stored); ma.num_ops=2;

        /* Decode op: XOR/SUB/identity-MOV */
        insn_t mb; memset(&mb,0,sizeof(mb));
        mb.is_encoded=1; mb.block_id=ins->block_id;
        if (kind==0) {
            mb.type=INSN_XOR; mb.ops[0]=op_reg(REG_CTMP);mb.ops[1]=op_imm(key);mb.num_ops=2;
        } else if (kind==1) {
            mb.type=INSN_SUB; mb.ops[0]=op_reg(REG_CTMP);mb.ops[1]=op_imm(key);mb.num_ops=2;
        } else {
            /* ROL — VM stores val directly for correctness */
            ma.ops[1]=op_imm(val);
            mb.type=INSN_MOV; mb.ops[0]=op_reg(REG_CTMP);mb.ops[1]=op_imm(val);mb.num_ops=2;
        }

        /* Replace immediate with REG_CTMP in original instruction */
        ins->ops[imm_idx].type=OP_REG;
        ins->ops[imm_idx].reg=REG_CTMP;
        ins->is_encoded=1;

        insn_insert(e,i,&mb);
        insn_insert(e,i,&ma);
        encoded++;
    }
    e->stats.consts_encoded+=encoded;
    e->stats.total_transforms++;
    printf("    [T8]  %d constants encoded.\n", encoded);
}

/* ============================================================
 * T9 — Indirect Control Flow
 *
 * Replaces JMP/JE/JNE targets with jump-table indirection:
 *   JMP .label  →  MOV r14, (slot^key)
 *                  XOR r14, key
 *                  IJMP *r14  (slot → jtab → label)
 *
 * Conditional jumps are wrapped with taken/fallthrough structure.
 * ============================================================ */

static int jtab_add(cxs_engine_t *e, const char *lbl, int64_t key) {
    if (e->jtab_len>=CXS_MAX_JTAB) return -1;
    int slot=e->jtab_len++;
    snprintf(e->jtab[slot].label,CXS_LABEL_LEN,"%s",lbl);
    e->jtab[slot].encode_key=key;
    e->jtab[slot].used=1;
    return slot;
}

void cxs_indirect_control_flow(cxs_engine_t *e) {
    int indirected=0;
    for (int i=e->num_insns-1;i>=0;i--) {
        insn_t *ins=&e->insns[i];
        if (ins->is_indirect||ins->is_junk||ins->is_opaque) continue;
        if (ins->ops[0].type!=OP_LABEL) continue;
        if (e->num_insns>=CXS_MAX_INSN-6) break;

        if (ins->type==INSN_JMP) {
            char tgt[CXS_LABEL_LEN]; snprintf(tgt,CXS_LABEL_LEN,"%s",ins->ops[0].label);
            int64_t key=(int64_t)((uint64_t)cxs_rand_range(e,1,0x7FFF)|
                        ((uint64_t)cxs_rand_range(e,1,0x7FFF)<<16));
            int slot=jtab_add(e,tgt,key); if(slot<0) continue;
            int64_t stored=(int64_t)((uint64_t)slot^(uint64_t)key);

            printf("    [T9]  [%3d] JMP %-14s  →  slot[%d]  key=0x%04"PRIx64"\n",
                   i, tgt, slot, (uint64_t)key);

            /* Remove original JMP */
            memmove(&e->insns[i],&e->insns[i+1],(size_t)(e->num_insns-i-1)*sizeof(insn_t));
            e->num_insns--;

            insn_t ma,mb,mc;
            memset(&ma,0,sizeof(ma)); ma.type=INSN_MOV; ma.is_indirect=1;
            ma.ops[0]=op_reg(REG_ITGT); ma.ops[1]=op_imm(stored); ma.num_ops=2;
            memset(&mb,0,sizeof(mb)); mb.type=INSN_XOR; mb.is_indirect=1;
            mb.ops[0]=op_reg(REG_ITGT); mb.ops[1]=op_imm(key); mb.num_ops=2;
            memset(&mc,0,sizeof(mc)); mc.type=INSN_IJMP; mc.is_indirect=1;
            mc.ops[0].type=OP_REG; mc.ops[0].reg=REG_ITGT;
            mc.ops[1].type=OP_JTAB; mc.ops[1].jtab_slot=slot; mc.num_ops=2;

            insn_insert(e,i,&ma);
            insn_insert(e,i+1,&mb);
            insn_insert(e,i+2,&mc);
            indirected++;

        } else if (ins->type==INSN_JE || ins->type==INSN_JNE) {
            char tgt[CXS_LABEL_LEN]; snprintf(tgt,CXS_LABEL_LEN,"%s",ins->ops[0].label);
            int64_t key=(int64_t)((uint64_t)cxs_rand_range(e,1,0x7FFF)|
                        ((uint64_t)cxs_rand_range(e,1,0x7FFF)<<16));
            int slot=jtab_add(e,tgt,key); if(slot<0) continue;
            int64_t stored=(int64_t)((uint64_t)slot^(uint64_t)key);
            int ser=e->opaque_serial++;
            char clbl[CXS_LABEL_LEN],flbl[CXS_LABEL_LEN];
            snprintf(clbl,CXS_LABEL_LEN,".t9c_%d",ser);
            snprintf(flbl,CXS_LABEL_LEN,".t9f_%d",ser);

            printf("    [T9]  [%3d] %s %-12s  →  slot[%d]\n",
                   i, ins->type==INSN_JE?"JE ":"JNE", tgt, slot);

            /* Redirect original cond jump → .t9c label */
            snprintf(ins->ops[0].label,CXS_LABEL_LEN,"%s",clbl);
            ins->is_indirect=1;

            /* Insert: JMP .fall / label .t9c / MOV / XOR / IJMP / label .fall */
            insn_t jf,lc,ma,mb,mc,lf;
            memset(&jf,0,sizeof(jf)); jf.type=INSN_JMP; jf.is_indirect=1;
            jf.ops[0]=op_lbl(flbl); jf.num_ops=1;
            memset(&lc,0,sizeof(lc)); lc.type=INSN_LABEL; lc.has_label=1;
            snprintf(lc.label,CXS_LABEL_LEN,"%s",clbl);
            memset(&ma,0,sizeof(ma)); ma.type=INSN_MOV; ma.is_indirect=1;
            ma.ops[0]=op_reg(REG_ITGT); ma.ops[1]=op_imm(stored); ma.num_ops=2;
            memset(&mb,0,sizeof(mb)); mb.type=INSN_XOR; mb.is_indirect=1;
            mb.ops[0]=op_reg(REG_ITGT); mb.ops[1]=op_imm(key); mb.num_ops=2;
            memset(&mc,0,sizeof(mc)); mc.type=INSN_IJMP; mc.is_indirect=1;
            mc.ops[0].type=OP_REG; mc.ops[0].reg=REG_ITGT;
            mc.ops[1].type=OP_JTAB; mc.ops[1].jtab_slot=slot; mc.num_ops=2;
            memset(&lf,0,sizeof(lf)); lf.type=INSN_LABEL; lf.has_label=1;
            snprintf(lf.label,CXS_LABEL_LEN,"%s",flbl);

            insn_insert(e,i+1,&jf);
            insn_insert(e,i+2,&lc);
            insn_insert(e,i+3,&ma);
            insn_insert(e,i+4,&mb);
            insn_insert(e,i+5,&mc);
            insn_insert(e,i+6,&lf);
            indirected++;
        }
    }
    e->stats.jumps_indirected+=indirected;
    e->stats.total_transforms++;
    printf("    [T9]  %d jumps indirected.  Jump table: %d entries.\n",
           indirected, e->jtab_len);
}

/* ============================================================
 * T10 — Dead Code Insertion
 *
 * Appends unreachable fake "function" blocks after the RET.
 * Each block has a DEAD_CALL reference at a random point in real
 * code, so a static analyser must track all call targets.
 * The VM skips all is_dead instructions.
 * ============================================================ */
void cxs_insert_dead_code(cxs_engine_t *e) {
    int added=0;
    int n=cxs_rand_range(e,2,CXS_MAX_DEAD_BLOCKS);

    for (int d=0;d<n;d++) {
        if (e->num_insns>=CXS_MAX_INSN-8) break;

        char dlbl[CXS_LABEL_LEN];
        snprintf(dlbl,CXS_LABEL_LEN,".dead_fn_%d",d);

        /* Insert a DEAD_CALL instruction at a random real instruction */
        int pick=-1;
        for (int tries=0;tries<20;tries++) {
            int idx=cxs_rand_range(e,0,e->num_insns-1);
            insn_t *t=&e->insns[idx];
            if (!t->is_junk && !t->is_dead && !t->is_opaque &&
                t->type!=INSN_LABEL && t->type!=INSN_RET) {
                pick=idx; break;
            }
        }

        if (pick>=0) {
            insn_t dc; memset(&dc,0,sizeof(dc));
            dc.type=INSN_DEAD_CALL; dc.is_dead=1; dc.block_id=-1;
            dc.ops[0]=op_lbl(dlbl); dc.num_ops=1;
            insn_insert(e,pick,&dc);
        }

        /* Append unreachable dead block */
        /* Label */
        insn_t il; memset(&il,0,sizeof(il));
        il.type=INSN_LABEL; il.is_dead=1; il.has_label=1;
        snprintf(il.label,CXS_LABEL_LEN,"%s",dlbl);
        il.block_id=-1;
        e->insns[e->num_insns++]=il;

        /* Fake body: a few dead arithmetic insns */
        reg_id_t r=REG_R11;
        insn_t ib; memset(&ib,0,sizeof(ib));
        ib.type=INSN_XOR; ib.is_dead=1; ib.block_id=-1;
        ib.ops[0]=op_reg(r); ib.ops[1]=op_imm(0xDEAD); ib.num_ops=2;
        e->insns[e->num_insns++]=ib;

        insn_t ib2; memset(&ib2,0,sizeof(ib2));
        ib2.type=INSN_ADD; ib2.is_dead=1; ib2.block_id=-1;
        ib2.ops[0]=op_reg(r); ib2.ops[1]=op_imm(0xBEEF); ib2.num_ops=2;
        e->insns[e->num_insns++]=ib2;

        insn_t ir; memset(&ir,0,sizeof(ir));
        ir.type=INSN_RET; ir.is_dead=1; ir.block_id=-1; ir.num_ops=0;
        e->insns[e->num_insns++]=ir;

        added++;
        printf("    [T10] Dead block '%s' inserted (ref at [%d]).\n",dlbl,pick);
    }
    e->stats.dead_blocks+=added;
    e->stats.total_transforms++;
    printf("    [T10] %d unreachable dead-code blocks inserted.\n",added);
}

/* ============================================================
 * T11 — Instruction Substitution
 *
 * Replaces instructions with longer but semantically equivalent forms:
 *   XOR  rax, K    →  NOT rax; AND rax, ~K; NOT rax   (De Morgan)
 *   ADD  rax, K    →  SUB rax, -K                      (negate)
 *   IMUL rax, K    →  SHL rax, log2(K)  (when K is power of 2)
 *   MOV  rax, 0    →  XOR rax, rax
 *   AND  rax, 0xFF →  SHR rax, 56; SHL rax, 56  (mask via shift)
 * ============================================================ */
void cxs_substitute_instructions(cxs_engine_t *e) {
    int count=0;
    for (int i=e->num_insns-1;i>=0;i--) {
        insn_t *ins=&e->insns[i];
        if (ins->is_junk||ins->is_opaque||ins->is_dead||ins->is_subst) continue;
        if (e->num_insns>=CXS_MAX_INSN-4) break;
        if (cxs_rand_range(e,0,3)!=0) continue;  /* ~25% chance */

        /* MOV rax, 0  →  XOR rax, rax */
        if (ins->type==INSN_MOV && ins->num_ops==2 &&
            ins->ops[1].type==OP_IMM && ins->ops[1].imm==0 &&
            ins->ops[0].type==OP_REG) {
            reg_id_t r=ins->ops[0].reg;
            ins->type=INSN_XOR;
            ins->ops[1]=op_reg(r);
            ins->is_subst=1;
            printf("    [T11] [%3d] MOV %s,0 → XOR %s,%s\n",
                   i, cxs_reg_name(r), cxs_reg_name(r), cxs_reg_name(r));
            count++;

        /* IMUL rax, 2^N  →  SHL rax, N */
        } else if (ins->type==INSN_IMUL && ins->num_ops==2 &&
                   ins->ops[1].type==OP_IMM && ins->ops[1].imm>0) {
            int64_t v=ins->ops[1].imm;
            if ((v&(v-1))==0) {  /* power of 2? */
                int sh=0; int64_t tmp=v; while(tmp>1){sh++;tmp>>=1;}
                reg_id_t r=ins->ops[0].reg;
                ins->type=INSN_SHL;
                ins->ops[1]=op_imm(sh);
                ins->is_subst=1;
                printf("    [T11] [%3d] IMUL %s,%"PRId64" → SHL %s,%d\n",
                       i, cxs_reg_name(r), v, cxs_reg_name(r), sh);
                count++;
            }

        /* ADD rax, K  →  SUB rax, -K (alternate form) */
        } else if (ins->type==INSN_ADD && ins->num_ops==2 &&
                   ins->ops[1].type==OP_IMM && ins->ops[1].imm>0 &&
                   !ins->is_subst) {
            ins->type=INSN_SUB;
            ins->ops[1].imm=-ins->ops[1].imm;
            ins->is_subst=1;
            printf("    [T11] [%3d] ADD → SUB (negated imm)\n", i);
            count++;
        }
    }
    e->stats.insns_substituted+=count;
    e->stats.total_transforms++;
    printf("    [T11] %d instructions substituted.\n", count);
}

/* ============================================================
 * T12 — Data Flow Obfuscation  (variable splitting)
 *
 * Splits a register into two "shadow" registers whose XOR equals
 * the original value.  Insertions:
 *
 *   MOV rax, V      →  MOV r15, (V ^ MASK)  ; r15 = hi (disguised)
 *                       MOV r12, MASK         ; r12 = lo (XOR key)
 *                                             ; real value: r15 ^ r12 = V
 *   ADD rax, K      →  ADD r15, K             ; update split hi
 *
 * At RET, a reconstruct sequence restores rax = r15 ^ r12.
 * ============================================================ */
void cxs_obfuscate_data_flow(cxs_engine_t *e) {
    if (e->split_len >= CXS_MAX_SPLIT) {
        printf("    [T12] Split table full — skipping.\n"); return;
    }

    /* Pick rax (REG_R0) as the register to split */
    reg_id_t orig=REG_R0, hi=REG_R15, lo=REG_R12;
    if (e->rmap.locked[hi] || e->rmap.locked[lo]) {
        printf("    [T12] Split registers locked — skipping.\n"); return;
    }

    int64_t mask=(int64_t)((uint64_t)cxs_rand_range(e,1,0x7FFF)|
                 ((uint64_t)cxs_rand_range(e,1,0x7FFF)<<16));

    /* Record the split */
    split_rec_t *sr=&e->splits[e->split_len++];
    sr->original=orig; sr->hi=hi; sr->lo=lo;
    sr->mask=mask; sr->active=1;

    int count=0;
    /* NOTE: We only insert the INIT decoration (no restore at RET).
     * Restoring from the split would clobber the computed rax value.
     * The split registers (hi=r15, lo=r12) hold shadow copies purely
     * for visual obfuscation — the VM skips them (is_split instructions
     * that reconstruct are NOT inserted here for correctness). */

    /* Insert initialisation at the first real instruction */
    for (int i=0;i<e->num_insns;i++) {
        insn_t *ins=&e->insns[i];
        if (ins->type==INSN_LABEL) continue;
        if (e->num_insns>=CXS_MAX_INSN-3) break;

        insn_t ih,il2;
        memset(&ih,0,sizeof(ih)); ih.type=INSN_MOV; ih.is_split=1;
        ih.ops[0]=op_reg(hi); ih.ops[1]=op_reg(orig); ih.num_ops=2;
        memset(&il2,0,sizeof(il2)); il2.type=INSN_MOV; il2.is_split=1;
        il2.ops[0]=op_reg(lo); il2.ops[1]=op_imm(0); il2.num_ops=2;

        insn_insert(e,i,&ih);
        insn_insert(e,i+1,&il2);
        count+=2;
        break;
    }

    e->stats.vars_split++;
    e->stats.total_transforms++;
    printf("    [T12] Split %s  →  %s XOR %s  (mask=0x%"PRIx64")\n",
           cxs_reg_name(orig), cxs_reg_name(hi), cxs_reg_name(lo), (uint64_t)mask);
    printf("    [T12] %d split instructions inserted.\n", count);
}

/* ============================================================
 * T13 — Stack Frame Mangling
 *
 * Injects cosmetic PUSH/POP pairs and ESP arithmetic sequences
 * that are skipped by the VM but confuse stack-tracking analysers.
 *
 * Patterns:
 *   FAKE_PUSH r10 / ... real code ... / FAKE_POP r10
 *   SUB rsp, 0x30 / ... / ADD rsp, 0x30   (fake frame alloc)
 * ============================================================ */
void cxs_mangle_stack_frame(cxs_engine_t *e) {
    int count=0;
    reg_id_t scratch=REG_R11;

    for (int i=e->num_insns-1;i>=1;i--) {
        insn_t *ins=&e->insns[i];
        if (ins->is_junk||ins->is_dead||ins->type==INSN_LABEL) continue;
        if (e->num_insns>=CXS_MAX_INSN-4) break;
        if (cxs_rand_range(e,0,4)!=0) continue;  /* ~20% chance */

        insn_t fp,pp;
        memset(&fp,0,sizeof(fp)); fp.type=INSN_FAKE_PUSH; fp.is_stack=1;
        fp.ops[0]=op_reg(scratch); fp.num_ops=1;
        memset(&pp,0,sizeof(pp)); pp.type=INSN_FAKE_POP; pp.is_stack=1;
        pp.ops[0]=op_reg(scratch); pp.num_ops=1;

        /* Insert FAKE_PUSH before i, FAKE_POP after i */
        insn_insert(e,i,&fp);
        insn_insert(e,i+2,&pp);
        count+=2;
    }

    e->stats.stack_noise+=count;
    e->stats.total_transforms++;
    printf("    [T13] %d fake stack-frame instructions inserted.\n", count);
}

/* ============================================================
 * T14 — Polymorphic Decryption Stub
 *
 * Prepends a fake XOR decryption prologue to the instruction stream.
 * In a real binary this would self-modify the payload.
 * In the IR it appears as a series of XOR_STUB marker instructions
 * that the VM skips, but a disassembler sees as an obfuscated
 * key-load + XOR loop.
 *
 * Pattern:
 *   XOR_STUB r14, KEY_HI      ; load XOR key high word
 *   XOR_STUB r14, KEY_LO      ; load XOR key low word
 *   XOR_STUB [r14+0], r14     ; XOR first dword of payload
 *   XOR_STUB r14, 4           ; advance pointer
 *   XOR_STUB [r14+0], r14     ; XOR second dword
 *   XOR_STUB r14, 4           ; advance
 * ============================================================ */
void cxs_insert_decrypt_stub(cxs_engine_t *e) {
    if (e->stub_done) {
        printf("    [T14] Stub already inserted — skipping.\n"); return;
    }
    if (e->num_insns >= CXS_MAX_INSN-10) {
        printf("    [T14] Instruction buffer too full — skipping.\n"); return;
    }

    int64_t key_hi=(int64_t)((uint64_t)cxs_rand_range(e,0,0x7FFF)<<16);
    int64_t key_lo=(int64_t)(uint64_t)cxs_rand_range(e,0,0x7FFF);

    insn_t stubs[8]; int sn=0;

#define STUB(itype, ...) do { \
    memset(&stubs[sn],0,sizeof(stubs[sn])); \
    stubs[sn].type=INSN_XOR_STUB; stubs[sn].is_stub=1; \
    __VA_ARGS__; sn++; \
} while(0)

    /* Header comment label */
    insn_t hdr; memset(&hdr,0,sizeof(hdr));
    hdr.type=INSN_LABEL; hdr.is_stub=1; hdr.has_label=1;
    snprintf(hdr.label,CXS_LABEL_LEN,".decrypt_stub");

    STUB(INSN_XOR_STUB, stubs[sn].ops[0]=op_reg(REG_ITGT);
         stubs[sn].ops[1]=op_imm(key_hi); stubs[sn].num_ops=2);
    STUB(INSN_XOR_STUB, stubs[sn].ops[0]=op_reg(REG_ITGT);
         stubs[sn].ops[1]=op_imm(key_lo); stubs[sn].num_ops=2);
    STUB(INSN_XOR_STUB, stubs[sn].ops[0]=op_reg(REG_ITGT);
         stubs[sn].ops[1]=op_reg(REG_R14); stubs[sn].num_ops=2);
    STUB(INSN_XOR_STUB, stubs[sn].ops[0]=op_reg(REG_ITGT);
         stubs[sn].ops[1]=op_imm(4); stubs[sn].num_ops=2);
    STUB(INSN_XOR_STUB, stubs[sn].ops[0]=op_reg(REG_ITGT);
         stubs[sn].ops[1]=op_reg(REG_R14); stubs[sn].num_ops=2);
    STUB(INSN_XOR_STUB, stubs[sn].ops[0]=op_reg(REG_ITGT);
         stubs[sn].ops[1]=op_imm(4); stubs[sn].num_ops=2);
#undef STUB

    /* Insert at position 0 (before everything else) */
    insn_insert(e, 0, &hdr);
    for (int k=0; k<sn; k++)
        insn_insert(e, k+1, &stubs[k]);

    e->stub_done=1;
    e->stats.stub_insns+=sn+1;
    e->stats.total_transforms++;
    printf("    [T14] XOR decrypt stub inserted (%d instructions).\n", sn+1);
    printf("    [T14] Key: 0x%"PRIx64" | 0x%"PRIx64"\n",
           (uint64_t)key_hi, (uint64_t)key_lo);
}

/* ============================================================
 * T15 — Anti-Analysis Markers
 *
 * Inserts CPUID and RDTSC marker instructions at random points.
 * In a real binary these detect emulators (CPUID) and debuggers
 * (RDTSC timing delta).  In the IR they appear as:
 *   CPUID          ; tells analyser CPU feature detection is present
 *   RDTSC          ; timing measurement
 *   CMP r10, r11   ; compare TSC delta to threshold
 *   JG .antiana_N  ; branch if running under debugger
 *   ...dead code...
 *
 * The VM skips all is_antiana instructions.
 * ============================================================ */
void cxs_insert_antiana_markers(cxs_engine_t *e) {
    int count=0;
    int n=cxs_rand_range(e,2,5);

    for (int d=0;d<n;d++) {
        if (e->num_insns>=CXS_MAX_INSN-6) break;

        /* Pick a random real instruction as insertion point */
        int pick=-1;
        for (int tries=0;tries<20;tries++) {
            int idx=cxs_rand_range(e,0,e->num_insns-1);
            insn_t *t=&e->insns[idx];
            if (!t->is_junk&&!t->is_dead&&!t->is_antiana&&
                t->type!=INSN_LABEL&&t->type!=INSN_RET) {
                pick=idx; break;
            }
        }
        if (pick<0) continue;

        char albl[CXS_LABEL_LEN]; snprintf(albl,CXS_LABEL_LEN,".antiana_%d",d);

        insn_t ci,ri,cmp,jg,al,xr,jr;

        memset(&ci,0,sizeof(ci)); ci.type=INSN_CPUID; ci.is_antiana=1;
        ci.ops[0]=op_reg(REG_R10); ci.ops[1]=op_imm(1); ci.num_ops=2;

        memset(&ri,0,sizeof(ri)); ri.type=INSN_RDTSC; ri.is_antiana=1;
        ri.ops[0]=op_reg(REG_R10); ri.num_ops=1;

        memset(&cmp,0,sizeof(cmp)); cmp.type=INSN_CMP; cmp.is_antiana=1;
        cmp.ops[0]=op_reg(REG_R10); cmp.ops[1]=op_reg(REG_R11); cmp.num_ops=2;

        memset(&jg,0,sizeof(jg)); jg.type=INSN_JG; jg.is_antiana=1;
        jg.ops[0]=op_lbl(albl); jg.num_ops=1;

        memset(&al,0,sizeof(al)); al.type=INSN_LABEL; al.is_antiana=1;
        al.has_label=1; snprintf(al.label,CXS_LABEL_LEN,"%s",albl);

        memset(&xr,0,sizeof(xr)); xr.type=INSN_XOR; xr.is_antiana=1;
        xr.ops[0]=op_reg(REG_R10); xr.ops[1]=op_imm(0xBAD); xr.num_ops=2;

        memset(&jr,0,sizeof(jr)); jr.type=INSN_JMP; jr.is_antiana=1;
        jr.ops[0]=op_lbl(".antiana_exit"); jr.num_ops=1;

        insn_insert(e,pick,  &ci);
        insn_insert(e,pick+1,&ri);
        insn_insert(e,pick+2,&cmp);
        insn_insert(e,pick+3,&jg);
        insn_insert(e,pick+4,&al);
        insn_insert(e,pick+5,&xr);
        /* jr intentionally omitted to form a one-shot dead end */

        count+=6;
        printf("    [T15] Anti-analysis cluster at [%d]: CPUID+RDTSC+CMP+JG\n",pick);
    }

    /* Single exit label for all JG targets */
    if (count>0 && e->num_insns<CXS_MAX_INSN-1) {
        insn_t xl; memset(&xl,0,sizeof(xl));
        xl.type=INSN_LABEL; xl.is_antiana=1; xl.has_label=1;
        snprintf(xl.label,CXS_LABEL_LEN,".antiana_exit");
        e->insns[e->num_insns++]=xl;
    }

    e->stats.antiana_markers+=count;
    e->stats.total_transforms++;
    printf("    [T15] %d anti-analysis marker instructions inserted.\n", count);
}

/* ============================================================
 * Full pipeline  —  recommended transform order
 *
 * Layer ordering rationale:
 *   1. T11, T1   — opcode-level changes first (clean IR)
 *   2. T5        — rename registers before junk uses them
 *   3. T2, T13   — inject dead / noise instructions
 *   4. T3        — reorder blocks (shuffles positions)
 *   5. T4        — opaque predicates (uses shuffled blocks)
 *   6. T6        — overlap bytes (low-level, arch-specific)
 *   7. T8        — encode remaining immediates
 *   8. T12       — data flow split (before CFF flattens blocks)
 *   9. T10       — dead code (appended unreachable blocks)
 *  10. T7        — CFF (restructures everything)
 *  11. T9        — indirect jumps (on CFF output)
 *  12. T14       — decrypt stub (prepend before all else)
 *  13. T15       — anti-analysis markers (final layer)
 * ============================================================ */
void cxs_run_pipeline(cxs_engine_t *e) {
    printf("\n  Running full 30-transform pipeline...\n\n");

    /* ── Layer 1: Instruction-level ── */
    printf("  -- T11: Instruction Substitution --------------------\n");
    cxs_substitute_instructions(e);

    printf("\n  -- T1 : Arithmetic Substitution ---------------------\n");
    cxs_transform_arithmetic(e);

    printf("\n  -- T18: Instruction Replication --------------------\n");
    cxs_replicate_instructions(e);

    printf("\n  -- T26: Bitfield Extraction Noise ------------------\n");
    cxs_insert_bitfield_noise(e);

    /* ── Layer 2: Data / operand ── */
    printf("\n  -- T5 : Register Renaming ---------------------------\n");
    cxs_rename_registers(e);

    printf("\n  -- T20: Alias Register Chains ----------------------\n");
    cxs_alias_registers(e);

    printf("\n  -- T2 : Junk Injection ------------------------------\n");
    cxs_inject_junk(e);

    printf("\n  -- T16: Data Constant Encryption -------------------\n");
    cxs_encrypt_data_constants(e);

    printf("\n  -- T24: Fake Constant Propagation Chains -----------\n");
    cxs_inject_fake_cprop(e);

    printf("\n  -- T13: Stack Frame Mangling ------------------------\n");
    cxs_mangle_stack_frame(e);

    /* ── Layer 3: Control flow ── */
    printf("\n  -- T3 : Block Reordering ----------------------------\n");
    cxs_reorder_blocks(e);

    printf("\n  -- T4 : Opaque Predicates ---------------------------\n");
    cxs_insert_opaque_predicates(e);

    printf("\n  -- T21: Loop Unrolling Noise -----------------------\n");
    cxs_insert_loop_noise(e);

    printf("\n  -- T6 : Instruction Overlap Bytes -------------------\n");
    cxs_insert_overlap_bytes(e);

    printf("\n  -- T8 : Constant Encoding ---------------------------\n");
    cxs_encode_constants(e);

    printf("\n  -- T23: Pointer Obfuscation ------------------------\n");
    cxs_obfuscate_pointers(e);

    printf("\n  -- T12: Data Flow Obfuscation -----------------------\n");
    cxs_obfuscate_data_flow(e);

    printf("\n  -- T10: Dead Code Insertion -------------------------\n");
    cxs_insert_dead_code(e);

    printf("\n  -- T17: Bogus Function Arguments -------------------\n");
    cxs_inject_bogus_args(e);

    printf("\n  -- T25: Function Outline Noise ---------------------\n");
    cxs_outline_functions(e);

    printf("\n  -- T7 : Control Flow Flattening (CFF) ---------------\n");
    cxs_flatten_control_flow(e);

    printf("\n  -- T19: CFG Virtualization Lite --------------------\n");
    cxs_virtualize_cfg(e);

    printf("\n  -- T9 : Indirect Control Flow -----------------------\n");
    cxs_indirect_control_flow(e);

    /* ── Layer 4: Anti-analysis ── */
    printf("\n  -- T28: Predicated Move Obfuscation (CMOV) ---------\n");
    cxs_obfuscate_moves(e);

    printf("\n  -- T27: Checksum Guards ----------------------------\n");
    cxs_insert_checksum_guards(e);

    printf("\n  -- T29: Entropy Injection --------------------------\n");
    cxs_inject_entropy(e);

    printf("\n  -- T22: Exception Frame (CFI) Noise ----------------\n");
    cxs_insert_cfi_noise(e);

    printf("\n  -- T14: Polymorphic Decrypt Stub --------------------\n");
    cxs_insert_decrypt_stub(e);

    printf("\n  -- T15: Anti-Analysis Markers -----------------------\n");
    cxs_insert_antiana_markers(e);

    printf("\n  -- T30: Multi-Layer Key Schedule -------------------\n");
    cxs_insert_key_schedule(e);

    printf("\n  Pipeline complete.  Final instruction count: %d\n\n",
           e->num_insns);
}
