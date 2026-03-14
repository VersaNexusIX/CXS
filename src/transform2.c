/*
 * transform2.c  —  CXS Transforms T16–T30
 * ============================================================
 * Implements the second layer of 15 obfuscation techniques
 * layered on top of T1–T15 from transform.c.
 *
 *  T16  Data encryption          rolling-XOR encryption of inline constants
 *  T17  Bogus function args      fake MOV arg-setup instructions
 *  T18  Instruction replication  semantically-neutral instruction clones
 *  T19  CFG virtualization lite  mini-VM bytecode handler dispatch
 *  T20  Alias register chains    r_alias=orig → use alias → restore
 *  T21  Loop unrolling noise     fake counted-loop (always 1 iter)
 *  T22  Exception frame noise    fake .cfi / SEH frame markers
 *  T23  Pointer obfuscation      base+encoded_offset address encoding
 *  T24  Constant propagation fake fake CProp candidate chains
 *  T25  Function outline noise   CALL+RET outline stubs
 *  T26  Bitfield extraction noise BSWAP/ROR/shift sequences on scratch
 *  T27  Checksum guards          rolling-XOR integrity check stubs
 *  T28  Predicated move obfus    MOV→CMOV ladder
 *  T29  Entropy injection        RDRAND/PRNG-seed sequences
 *  T30  Multi-layer key schedule multi-round XOR-ADD-ROL key expansion
 * ============================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cxs.h"

/* ============================================================
 * Internal helpers  (mirror transform.c conventions)
 * ============================================================ */

static uint64_t t2_rand(cxs_engine_t *e) {
    e->seed ^= e->seed << 13;
    e->seed ^= e->seed >> 7;
    e->seed ^= e->seed << 17;
    return e->seed;
}
static int t2_range(cxs_engine_t *e, int lo, int hi) {
    if (lo >= hi) return lo;
    return lo + (int)(t2_rand(e) % (uint64_t)(hi - lo));
}

/* Build a blank instruction */
static insn_t blank_insn(void) {
    insn_t i;
    memset(&i, 0, sizeof(i));
    i.block_id = -1;
    return i;
}

static operand_t op_reg(reg_id_t r)  { operand_t o; memset(&o,0,sizeof(o)); o.type=OP_REG; o.reg=r; return o; }
static operand_t op_imm(int64_t v)   { operand_t o; memset(&o,0,sizeof(o)); o.type=OP_IMM; o.imm=v; return o; }
static operand_t op_lbl(const char *s){ operand_t o; memset(&o,0,sizeof(o)); o.type=OP_LABEL; snprintf(o.label,CXS_LABEL_LEN,"%s",s); return o; }

/* Insert instruction BEFORE position pos (shifts everything right) */
static int insn_insert_before(cxs_engine_t *e, int pos, insn_t *ins) {
    if (e->num_insns >= CXS_MAX_INSN - 1) return -1;
    memmove(&e->insns[pos+1], &e->insns[pos],
            sizeof(insn_t) * (size_t)(e->num_insns - pos));
    e->insns[pos] = *ins;
    e->num_insns++;
    return 0;
}

/* Append instruction at end */
static int insn_append(cxs_engine_t *e, insn_t *ins) {
    if (e->num_insns >= CXS_MAX_INSN - 1) return -1;
    e->insns[e->num_insns++] = *ins;
    return 0;
}

/* Scratch registers usable by decoration transforms */
static const reg_id_t SCRATCH[] = { REG_R10, REG_R11, REG_R12 };
static const int      NSCRATCH  = 3;

/* ============================================================
 * T16 — Data Constant Encryption
 *
 * Strategy: find all INSN_IMM operands whose absolute value > 4.
 * Replace each with a 3-instruction decode sequence:
 *   MOV  scratch, (encrypted_val)
 *   XOR  scratch, rolling_key
 *   ADD  dst, scratch
 * The rolling key advances by a PRNG step each constant.
 * Mark instructions is_dataenc=1.
 * VM support: VM already handles decoded values correctly
 * because we only encrypt constants that are ADDED/SUBBED
 * from the result register — and we fold the decode inline.
 * ============================================================ */
void cxs_encrypt_data_constants(cxs_engine_t *e) {
    int count = 0;
    int64_t roll_key = (int64_t)(t2_rand(e) & 0xFFFFFFFF) | 1;

    /* Only encrypt ADD/SUB/MOV with immediate > 4 that are NOT
     * already encoded by T8 (is_encoded) or are junk/dead */
    for (int i = 0; i < e->num_insns && count < 6; i++) {
        insn_t *ins = &e->insns[i];
        if (ins->is_encoded || ins->is_junk || ins->is_dead ||
            ins->is_stub   || ins->is_antiana || ins->is_dataenc)
            continue;
        if (ins->type != INSN_ADD && ins->type != INSN_SUB &&
            ins->type != INSN_MOV)
            continue;
        /* find an immediate operand */
        int op_idx = -1;
        for (int k = 0; k < ins->num_ops; k++) {
            if (ins->ops[k].type == OP_IMM &&
                (ins->ops[k].imm > 4 || ins->ops[k].imm < -4)) {
                op_idx = k; break;
            }
        }
        if (op_idx < 0) continue;

        int64_t  orig_val = ins->ops[op_idx].imm;
        int64_t  enc_val  = orig_val ^ roll_key;
        reg_id_t scratch  = SCRATCH[t2_range(e, 0, NSCRATCH)];
        roll_key = (roll_key * 6364136223846793005LL) + 1442695040888963407LL;

        /* Build decode block: MOV scratch, enc | XOR scratch, key |
         * replace original operand with scratch reg */
        insn_t m1 = blank_insn();
        m1.type       = INSN_MOV;
        m1.is_dataenc = 1;
        m1.block_id   = ins->block_id;
        m1.ops[0]     = op_reg(scratch);
        m1.ops[1]     = op_imm(enc_val);
        m1.num_ops    = 2;

        insn_t m2 = blank_insn();
        m2.type       = INSN_XOR;
        m2.is_dataenc = 1;
        m2.block_id   = ins->block_id;
        m2.ops[0]     = op_reg(scratch);
        m2.ops[1]     = op_imm(roll_key ^ (roll_key ^ orig_val ^ enc_val));
        /* m2 XOR key recovers: enc_val ^ key = orig_val
         * key for XOR step = enc_val ^ orig_val = roll_key_orig */
        m2.ops[1]     = op_imm(roll_key & 0x7FFFFFFF); /* visible key */
        m2.num_ops    = 2;
        /* Rewrite: actual XOR key so scratch = orig_val after:
         * scratch = enc_val; scratch ^= xorkey  → xorkey = enc_val^orig_val */
        m2.ops[1].imm = enc_val ^ orig_val;

        /* Insert m1, m2 before current instruction */
        if (insn_insert_before(e, i, &m2) < 0) break;
        if (insn_insert_before(e, i, &m1) < 0) break;
        i += 2;  /* skip past inserted instructions */

        /* Replace the original immediate operand with the scratch register */
        e->insns[i].ops[op_idx] = op_reg(scratch);
        e->insns[i].is_dataenc  = 1;

        printf("    [T16] insn[%d] imm %"PRId64" → enc(0x%"PRIx64") xor key=0x%"PRIx64"\n",
               i, orig_val, (uint64_t)enc_val, (uint64_t)(enc_val ^ orig_val));
        count++;
        roll_key = (roll_key ^ (uint64_t)orig_val) * 0x9e3779b97f4a7c15ULL;
    }

    e->stats.data_encrypted += count;
    printf("    [T16] %d constant(s) data-encrypted.\n", count);
}

/* ============================================================
 * T17 — Bogus Function Argument Injection
 *
 * Inserts fake argument-setup instructions (MOV arg_reg, fake_val)
 * at the entry block. These look like function call preambles to
 * static analysis but have no effect on the real computation
 * since arg_regs (rsi/rdx/rcx etc.) are never read by the function.
 * ============================================================ */
void cxs_inject_bogus_args(cxs_engine_t *e) {

    /* Arg registers we can clobber (not rax=result, not rsp/rbp) */
    static const reg_id_t ARG_REGS[] = {
        REG_R1, REG_R2, REG_R3, REG_R4  /* rbx, rcx, rdx, rsi */
    };
    static const int N_ARGS = 4;

    int num_inject = t2_range(e, 2, 5);
    int count      = 0;

    /* Find insertion point: after any LABEL at insn 0 */
    int insert_pos = 0;
    while (insert_pos < e->num_insns &&
           (e->insns[insert_pos].type == INSN_LABEL ||
            e->insns[insert_pos].is_stub ||
            e->insns[insert_pos].is_keysched))
        insert_pos++;

    for (int k = 0; k < num_inject && count < N_ARGS; k++) {
        reg_id_t ar  = ARG_REGS[k % N_ARGS];
        int64_t  val = (int64_t)(t2_rand(e) & 0xFFFFFF) + 1;

        insn_t ins = blank_insn();
        ins.type      = INSN_BOGUS_ARG;
        ins.is_bogus  = 1;
        ins.block_id  = (e->num_insns > 0) ? e->insns[insert_pos > 0 ? insert_pos-1 : 0].block_id : 0;
        ins.ops[0]    = op_reg(ar);
        ins.ops[1]    = op_imm(val);
        ins.num_ops   = 2;

        if (insn_insert_before(e, insert_pos + count, &ins) < 0) break;
        printf("    [T17] bogus arg: %s = 0x%"PRIx64"\n",
               cxs_reg_name(ar), (uint64_t)val);
        count++;
    }

    e->stats.bogus_args += count;
    printf("    [T17] %d bogus argument instruction(s) injected.\n", count);
}

/* ============================================================
 * T18 — Instruction Replication
 *
 * Finds semantically-neutral instructions (NOP, CMP that sets
 * flags but result is unused, OR/AND with identity operands)
 * and inserts identical copies nearby. Creates multiple
 * semantically identical but physically distinct code paths
 * that confuse pattern matching.
 * ============================================================ */
void cxs_replicate_instructions(cxs_engine_t *e) {
    int count = 0;

    for (int i = 0; i < e->num_insns - 1 && count < 8; i++) {
        insn_t *ins = &e->insns[i];
        /* Only replicate NOPs or pure junk instructions */
        if (ins->type != INSN_NOP && !ins->is_junk) continue;
        if (ins->is_dead || ins->is_outlined) continue;

        /* Clone and insert after current position */
        insn_t clone = *ins;
        clone.is_replicated = 1;
        /* Slightly vary: for NOP, optionally use junk XOR r,r */
        if (t2_range(e, 0, 2)) {
            reg_id_t r = SCRATCH[t2_range(e, 0, NSCRATCH)];
            clone.type    = INSN_XOR;
            clone.is_junk = 1;
            clone.ops[0]  = op_reg(r);
            clone.ops[1]  = op_reg(r);
            clone.num_ops = 2;
        }
        int pos = i + 1;
        if (insn_insert_before(e, pos, &clone) < 0) break;
        i++;  /* skip cloned instruction */
        count++;
    }

    e->stats.insns_replicated += count;
    printf("    [T18] %d instruction(s) replicated.\n", count);
}

/* ============================================================
 * T19 — CFG Virtualization Lite
 *
 * Wraps a small subset of real instructions inside a
 * mini-VM dispatch pattern:
 *
 *   MOV  vpc, VOPCODE_ENTRY         ; load virtual PC
 * .virt_dispatch:
 *   CMP  vpc, VOPCODE_N
 *   JE   .vhandler_N
 *   ...
 * .vhandler_N:
 *   <real instruction>
 *   MOV  vpc, VOPCODE_NEXT
 *   JMP  .virt_dispatch
 * .virt_done:
 *
 * This makes each real instruction look like a dispatch table
 * case, massively complicating static analysis.
 *
 * VM impact: the virtualized instructions are marked is_virt=1.
 * The VM skips VIRT_ENTER/VIRT_EXIT wrapper instructions and
 * executes VIRT_OP instructions directly (they carry the real op).
 * ============================================================ */
void cxs_virtualize_cfg(cxs_engine_t *e) {

    if (e->virt_done) {
        printf("    [T19] Already applied, skipping.\n");
        return;
    }

    /* Find a small run of real arithmetic instructions to virtualize */
    int start = -1, virt_len = 0;
    for (int i = 0; i < e->num_insns; i++) {
        insn_t *ins = &e->insns[i];
        if (!ins->is_junk && !ins->is_dead && !ins->is_stub &&
            !ins->is_antiana && !ins->is_flat && !ins->is_virt &&
            !ins->is_opaque && !ins->is_stack &&
            (ins->type == INSN_ADD || ins->type == INSN_SUB ||
             ins->type == INSN_MUL || ins->type == INSN_IMUL) &&
            ins->num_ops == 2) {
            if (start < 0) start = i;
            virt_len++;
            if (virt_len >= 3) break;
        } else if (start >= 0) {
            break;
        }
    }

    if (start < 0 || virt_len < 1) {
        printf("    [T19] No suitable instruction run found.\n");
        return;
    }
    if (virt_len > 3) virt_len = 3;

    /* Generate labels */
    char lbl_disp[CXS_LABEL_LEN], lbl_done[CXS_LABEL_LEN];
    snprintf(lbl_disp, sizeof(lbl_disp), ".virt_dispatch_%d", e->opaque_serial);
    snprintf(lbl_done, sizeof(lbl_done), ".virt_done_%d",     e->opaque_serial);
    e->opaque_serial++;

    reg_id_t vpc = REG_R10;  /* virtual PC register */

    /* Save: we'll insert the entire preamble before `start` */
    /* Step 1: insert VIRT_ENTER + initial MOV vpc, 0 */
    {
        insn_t ve = blank_insn();
        ve.type     = INSN_VIRT_ENTER;
        ve.is_virt  = 1;
        ve.block_id = e->insns[start].block_id;
        snprintf(ve.label, CXS_LABEL_LEN, "%s", lbl_disp);
        ve.has_label = 1;
        if (insn_insert_before(e, start, &ve) < 0) goto done;
        start++;

        insn_t mv = blank_insn();
        mv.type    = INSN_MOV;
        mv.is_virt = 1;
        mv.block_id = ve.block_id;
        mv.ops[0]  = op_reg(vpc);
        mv.ops[1]  = op_imm(0);   /* vopcode 0 = first handler */
        mv.num_ops = 2;
        if (insn_insert_before(e, start, &mv) < 0) goto done;
        start++;
    }

    /* Step 2: for each virtualized instruction, insert:
     *   CMP vpc, VOPCODE_N ; JE .vhandler_N
     * Then at the target instruction, wrap with label + VIRT_OP + jump back */
    for (int h = 0; h < virt_len; h++) {
        char lbl_h[CXS_LABEL_LEN];
        snprintf(lbl_h, sizeof(lbl_h), ".vhandler_%d_%d", e->opaque_serial - 1, h);

        /* Dispatch: CMP vpc, h */
        insn_t cmp = blank_insn();
        cmp.type    = INSN_CMP;
        cmp.is_virt = 1;
        cmp.block_id = e->insns[start - 1].block_id;
        cmp.ops[0]  = op_reg(vpc);
        cmp.ops[1]  = op_imm(h);
        cmp.num_ops = 2;
        if (insn_insert_before(e, start, &cmp) < 0) goto done;
        start++;

        /* JE .vhandler_h */
        insn_t je = blank_insn();
        je.type    = INSN_JE;
        je.is_virt = 1;
        je.block_id = cmp.block_id;
        je.ops[0]  = op_lbl(lbl_h);
        je.num_ops = 1;
        if (insn_insert_before(e, start, &je) < 0) goto done;
        start++;

        /* The actual instruction is now at start + virt_len (h-th real insn
         * was shifted by 2 for each h inserted before it)
         * We skip modifying the original — just wrap it by inserting
         * a label anchor before it and a jump-back after it */
        int real_pos = start + (virt_len - h - 1); /* real insn still after preamble */
        /* Simpler: just record the handler label; mark the original as virt */
        /* We'll add labels during a second pass */
        e->insns[real_pos].is_virt = 1;
        if (!e->insns[real_pos].has_label) {
            snprintf(e->insns[real_pos].label, CXS_LABEL_LEN, "%s", lbl_h);
            e->insns[real_pos].has_label = 1;
        }

        /* After the real instruction, insert: MOV vpc, h+1; JMP .virt_dispatch */
        int after_pos = real_pos + 1;
        insn_t nv = blank_insn();
        nv.type    = INSN_MOV;
        nv.is_virt = 1;
        nv.block_id = e->insns[real_pos].block_id;
        nv.ops[0]  = op_reg(vpc);
        nv.ops[1]  = op_imm(h + 1);
        nv.num_ops = 2;
        if (insn_insert_before(e, after_pos, &nv) < 0) goto done;

        insn_t jmp = blank_insn();
        jmp.type    = (h < virt_len - 1) ? INSN_JMP : INSN_JMP;
        jmp.is_virt = 1;
        jmp.block_id = nv.block_id;
        jmp.ops[0]  = (h < virt_len - 1)
                      ? op_lbl(lbl_disp)
                      : op_lbl(lbl_done);
        jmp.num_ops = 1;
        if (insn_insert_before(e, after_pos + 1, &jmp) < 0) goto done;

        if (e->vhandler_len < CXS_MAX_HANDLERS) {
            snprintf(e->vhandlers[e->vhandler_len].label,
                     CXS_LABEL_LEN, "%s", lbl_h);
            e->vhandlers[e->vhandler_len].opcode = h;
            e->vhandlers[e->vhandler_len].used   = 1;
            e->vhandler_len++;
        }
    }

    /* Step 3: insert JMP .virt_done after last dispatch CMP (unreached path) */
    {
        insn_t jd = blank_insn();
        jd.type    = INSN_JMP;
        jd.is_virt = 1;
        jd.block_id = e->insns[start-1].block_id;
        jd.ops[0]  = op_lbl(lbl_done);
        jd.num_ops = 1;
        if (insn_insert_before(e, start, &jd) < 0) goto done;
        start++;
    }

    /* Step 4: insert VIRT_EXIT after all handlers */
    {
        /* Find end of last handler (after the last jmp we inserted) */
        insn_t vx = blank_insn();
        vx.type      = INSN_VIRT_EXIT;
        vx.is_virt   = 1;
        vx.block_id  = e->insns[start].block_id;
        snprintf(vx.label, CXS_LABEL_LEN, "%s", lbl_done);
        vx.has_label = 1;
        /* Append at current position */
        (void)insn_append(e, &vx);
    }

done:
    e->virt_done = 1;
    e->stats.virt_handlers += virt_len;
    printf("    [T19] %d instruction(s) wrapped in mini-VM dispatch.\n", virt_len);
    printf("    [T19] Dispatch label: %s  Done: %s\n", lbl_disp, lbl_done);
}

/* ============================================================
 * T20 — Alias Register Chains
 *
 * For each scratch register r in the pool, inserts a chain:
 *   MOV  alias, r         ; r_alias = r_orig
 *   <uses of r replaced with alias>
 *   MOV  r, alias         ; restore (semantically a NOP if no diverge)
 *
 * The alias chain is purely decorative — the real computation
 * uses the original register; the alias copy confuses
 * register liveness analysis.
 * ============================================================ */
void cxs_alias_registers(cxs_engine_t *e) {
    int count = 0;

    /* Only alias r10 → r11 pair (r12 reserved for T12) */
    static const reg_id_t SRC[]   = { REG_R10 };
    static const reg_id_t ALIAS[] = { REG_R11 };
    static const int N_PAIR = 1;

    for (int p = 0; p < N_PAIR && count < 2; p++) {
        reg_id_t src   = SRC[p];
        reg_id_t alias = ALIAS[p];

        /* Find a range of instructions where src is read at least once */
        int first = -1, last = -1;
        for (int i = 0; i < e->num_insns; i++) {
            insn_t *ins = &e->insns[i];
            if (ins->is_dead || ins->is_stub || ins->is_antiana) continue;
            for (int k = 0; k < ins->num_ops; k++) {
                if (ins->ops[k].type == OP_REG && ins->ops[k].reg == src) {
                    if (first < 0) first = i;
                    last = i;
                }
            }
        }
        if (first < 0 || last <= first) continue;
        if (last - first < 2) continue;

        /* Insert: MOV alias, src  at `first` */
        insn_t mv_in = blank_insn();
        mv_in.type     = INSN_MOV;
        mv_in.is_alias = 1;
        mv_in.block_id = e->insns[first].block_id;
        mv_in.ops[0]   = op_reg(alias);
        mv_in.ops[1]   = op_reg(src);
        mv_in.num_ops  = 2;
        if (insn_insert_before(e, first, &mv_in) < 0) break;
        last++;  /* shift */

        /* Insert: MOV src, alias  at `last+1` (restore) */
        insn_t mv_out = blank_insn();
        mv_out.type     = INSN_MOV;
        mv_out.is_alias = 1;
        mv_out.block_id = e->insns[last].block_id;
        mv_out.ops[0]   = op_reg(src);
        mv_out.ops[1]   = op_reg(alias);
        mv_out.num_ops  = 2;
        if (insn_insert_before(e, last + 1, &mv_out) < 0) break;

        /* Record alias pair */
        if (e->alias_len < CXS_MAX_ALIAS) {
            e->aliases[e->alias_len].original = src;
            e->aliases[e->alias_len].alias    = alias;
            e->aliases[e->alias_len].active   = 1;
            e->alias_len++;
        }

        printf("    [T20] Alias chain: %s → %s  (insn %d..%d)\n",
               cxs_reg_name(src), cxs_reg_name(alias), first, last);
        count++;
    }

    e->stats.alias_chains += count;
    printf("    [T20] %d alias chain(s) inserted.\n", count);
}

/* ============================================================
 * T21 — Loop Unrolling Noise
 *
 * Inserts a fake counted-loop structure that always executes
 * exactly once (counter initialized to 1, loop condition is
 * "counter > 0", counter decremented → exits after 1 iter).
 *
 * Structure:
 *   MOV  rcx, 1          ; loop counter = 1
 * .loop_N:
 *   <real instruction(s)>
 *   SUB  rcx, 1
 *   JNZ  .loop_N         ; never taken (rcx=0 after first iter)
 * .loop_N_exit:
 *
 * This looks like an unrolled loop to static analysis, triggers
 * false loop-detection heuristics, and wastes decompiler time.
 * ============================================================ */
void cxs_insert_loop_noise(cxs_engine_t *e) {
    int count = 0;

    for (int i = 2; i < e->num_insns - 4 && count < 2; i++) {
        insn_t *ins = &e->insns[i];
        /* Find a real arithmetic instruction to wrap */
        if (ins->is_junk || ins->is_dead || ins->is_stub || ins->is_virt ||
            ins->is_opaque || ins->is_antiana || ins->is_loop || ins->is_flat)
            continue;
        if (ins->type != INSN_ADD && ins->type != INSN_SUB &&
            ins->type != INSN_XOR)
            continue;

        char lbl_top[CXS_LABEL_LEN], lbl_exit[CXS_LABEL_LEN];
        snprintf(lbl_top,  sizeof(lbl_top),  ".loop_%d",      e->loop_serial);
        snprintf(lbl_exit, sizeof(lbl_exit), ".loop_%d_exit", e->loop_serial);
        e->loop_serial++;

        reg_id_t ctr = REG_R8;   /* use r8 as loop counter */
        int      blk = ins->block_id;

        /* 1. MOV ctr, 1 — before the instruction */
        insn_t m_init = blank_insn();
        m_init.type    = INSN_LOOP_INIT;
        m_init.is_loop = 1;
        m_init.block_id = blk;
        m_init.ops[0]  = op_reg(ctr);
        m_init.ops[1]  = op_imm(1);
        m_init.num_ops = 2;
        snprintf(m_init.label, CXS_LABEL_LEN, "%s", lbl_top);
        m_init.has_label = 1;
        if (insn_insert_before(e, i, &m_init) < 0) break;
        i++;   /* real insn now at i */

        /* 2. After the real instruction: SUB ctr, 1 */
        insn_t m_sub = blank_insn();
        m_sub.type    = INSN_SUB;
        m_sub.is_loop = 1;
        m_sub.block_id = blk;
        m_sub.ops[0]  = op_reg(ctr);
        m_sub.ops[1]  = op_imm(1);
        m_sub.num_ops = 2;
        if (insn_insert_before(e, i + 1, &m_sub) < 0) break;

        /* 3. JNE .loop_top (never taken since ctr==0) */
        insn_t m_jne = blank_insn();
        m_jne.type     = INSN_LOOP_CMP;
        m_jne.is_loop  = 1;
        m_jne.block_id = blk;
        m_jne.ops[0]   = op_lbl(lbl_top);
        m_jne.num_ops  = 1;
        if (insn_insert_before(e, i + 2, &m_jne) < 0) break;

        /* 4. NOP with exit label */
        insn_t m_exit = blank_insn();
        m_exit.type      = INSN_NOP;
        m_exit.is_loop   = 1;
        m_exit.block_id  = blk;
        snprintf(m_exit.label, CXS_LABEL_LEN, "%s", lbl_exit);
        m_exit.has_label = 1;
        if (insn_insert_before(e, i + 3, &m_exit) < 0) break;

        printf("    [T21] Loop noise around insn[%d]: %s / %s\n",
               i, lbl_top, lbl_exit);
        i += 4;
        count++;
    }

    e->stats.loop_noise += count;
    printf("    [T21] %d fake loop structure(s) inserted.\n", count);
}

/* ============================================================
 * T22 — Exception Frame / CFI Noise
 *
 * Emits fake DWARF CFI / SEH unwind markers as label-anchored
 * NOP instructions. In the emitted .S these become .cfi_*
 * directives that confuse unwinders and decompilers.
 * The markers are VM-invisible (NOP type).
 * ============================================================ */
void cxs_insert_cfi_noise(cxs_engine_t *e) {
    int count = 0;

    /* Insert .cfi_startproc at position 0, .cfi_endproc at end */
    {
        insn_t cfi_s = blank_insn();
        cfi_s.type      = INSN_CFI_START;
        cfi_s.is_cfi    = 1;
        cfi_s.block_id  = e->num_insns > 0 ? e->insns[0].block_id : 0;
        snprintf(cfi_s.label, CXS_LABEL_LEN, ".cfi_startproc");
        cfi_s.has_label = 1;
        if (insn_insert_before(e, 0, &cfi_s) >= 0) count++;
    }
    {
        insn_t cfi_e = blank_insn();
        cfi_e.type      = INSN_CFI_END;
        cfi_e.is_cfi    = 1;
        cfi_e.block_id  = e->insns[e->num_insns-1].block_id;
        snprintf(cfi_e.label, CXS_LABEL_LEN, ".cfi_endproc");
        cfi_e.has_label = 1;
        if (insn_append(e, &cfi_e) >= 0) count++;
    }

    /* Insert 2 fake .cfi_def_cfa_offset markers mid-stream */
    int mid = e->num_insns / 3;
    for (int k = 0; k < 2 && count < 8; k++) {
        int pos = mid + k * (e->num_insns / 4);
        if (pos >= e->num_insns) break;
        insn_t cfi_m = blank_insn();
        cfi_m.type      = INSN_CFI_START;
        cfi_m.is_cfi    = 1;
        cfi_m.block_id  = e->insns[pos].block_id;
        snprintf(cfi_m.label, CXS_LABEL_LEN,
                 ".cfi_def_cfa_offset %d", (k+1) * 16);
        cfi_m.has_label = 1;
        if (insn_insert_before(e, pos, &cfi_m) >= 0) count++;
    }

    e->stats.cfi_markers += count;
    printf("    [T22] %d CFI/exception frame marker(s) inserted.\n", count);
}

/* ============================================================
 * T23 — Pointer Obfuscation
 *
 * For any MEM operand (base+disp), encodes the displacement as:
 *   real_disp = (encoded_disp ^ mask) - adjustment
 *
 * Inserts a decode sequence before the memory access:
 *   MOV  scratch, encoded_disp
 *   XOR  scratch, mask
 *   SUB  scratch, adjustment
 *   <original insn with disp replaced by [base + scratch]>
 *
 * Purely decorative in the IR VM (VM evaluates disp directly)
 * but emitted assembly will contain the actual decode sequence.
 * ============================================================ */
void cxs_obfuscate_pointers(cxs_engine_t *e) {
    int count = 0;

    for (int i = 0; i < e->num_insns - 1 && count < 4; i++) {
        insn_t *ins = &e->insns[i];
        if (ins->is_dead || ins->is_stub || ins->is_junk || ins->is_ptrenc)
            continue;
        /* Find a MEM operand with non-zero displacement */
        for (int k = 0; k < ins->num_ops && count < 4; k++) {
            if (ins->ops[k].type != OP_MEM) continue;
            int64_t disp = ins->ops[k].mem.disp;
            if (disp == 0) continue;

            int64_t mask = (int64_t)(t2_rand(e) & 0xFF) | 0x55;
            int64_t enc  = disp ^ mask;

            reg_id_t sc = SCRATCH[t2_range(e, 0, NSCRATCH)];
            int      blk = ins->block_id;

            /* MOV sc, enc */
            insn_t m1 = blank_insn();
            m1.type      = INSN_MOV;
            m1.is_ptrenc = 1;
            m1.block_id  = blk;
            m1.ops[0]    = op_reg(sc);
            m1.ops[1]    = op_imm(enc);
            m1.num_ops   = 2;

            /* XOR sc, mask */
            insn_t m2 = blank_insn();
            m2.type      = INSN_XOR;
            m2.is_ptrenc = 1;
            m2.block_id  = blk;
            m2.ops[0]    = op_reg(sc);
            m2.ops[1]    = op_imm(mask);
            m2.num_ops   = 2;

            if (insn_insert_before(e, i, &m1) < 0) goto ptr_done;
            if (insn_insert_before(e, i+1, &m2) < 0) goto ptr_done;
            i += 2;

            e->insns[i].is_ptrenc = 1;

            printf("    [T23] disp %"PRId64" → enc 0x%"PRIx64" mask 0x%"PRIx64"\n",
                   disp, (uint64_t)enc, (uint64_t)mask);
            count++;
        }
    }
ptr_done:
    e->stats.ptrs_encoded += count;
    printf("    [T23] %d pointer displacement(s) obfuscated.\n", count);
}

/* ============================================================
 * T24 — Fake Constant Propagation Chains
 *
 * Inserts instruction chains that look like ideal candidates
 * for constant propagation but are subtly non-constant:
 *
 *   MOV  sc, K1
 *   ADD  sc, K2           ; sc = K1+K2  (looks constant)
 *   XOR  sc, sc           ; sc = 0      (defeats propagation!)
 *   OR   sc, K3           ; sc = K3
 *   <junk use of sc>
 *
 * A constant-propagation pass will "simplify" this to sc=K3,
 * but then hit the XOR sc,sc which invalidates the chain.
 * Classic CProp poison.
 * ============================================================ */
void cxs_inject_fake_cprop(cxs_engine_t *e) {
    int count = 0;

    for (int i = 5; i < e->num_insns - 10 && count < 3; i++) {
        insn_t *ins = &e->insns[i];
        if (ins->is_dead || ins->is_junk || ins->is_stub) continue;
        if (ins->type != INSN_NOP && !ins->is_junk) continue;

        reg_id_t sc  = SCRATCH[t2_range(e, 0, NSCRATCH)];
        int64_t  k1  = (int64_t)(t2_rand(e) & 0xFFF) + 100;
        int64_t  k2  = (int64_t)(t2_rand(e) & 0xFF)  + 1;
        int64_t  k3  = (int64_t)(t2_rand(e) & 0xFF)  + 200;
        int      blk = ins->block_id;

        insn_t chain[5];
        for (int c = 0; c < 5; c++) { chain[c] = blank_insn(); chain[c].is_fakecp = 1; chain[c].block_id = blk; }

        chain[0].type = INSN_MOV; chain[0].ops[0]=op_reg(sc); chain[0].ops[1]=op_imm(k1); chain[0].num_ops=2;
        chain[1].type = INSN_ADD; chain[1].ops[0]=op_reg(sc); chain[1].ops[1]=op_imm(k2); chain[1].num_ops=2;
        chain[2].type = INSN_XOR; chain[2].ops[0]=op_reg(sc); chain[2].ops[1]=op_reg(sc); chain[2].num_ops=2; /* sc=0 */
        chain[3].type = INSN_OR;  chain[3].ops[0]=op_reg(sc); chain[3].ops[1]=op_imm(k3); chain[3].num_ops=2;
        chain[4].type = INSN_NOP; chain[4].is_junk = 1;

        int inserted = 0;
        for (int c = 0; c < 5; c++) {
            if (insn_insert_before(e, i + inserted, &chain[c]) < 0) break;
            inserted++;
        }
        i += inserted;

        printf("    [T24] Fake CProp chain at [%d]: sc=%s  K=%"PRId64"+%"PRId64"+XOR+%"PRId64"\n",
               i - inserted, cxs_reg_name(sc), k1, k2, k3);
        count++;
    }

    e->stats.fakecp_chains += count;
    printf("    [T24] %d fake constant-propagation chain(s) injected.\n", count);
}

/* ============================================================
 * T25 — Function Outline Noise
 *
 * Takes a small instruction from mid-function and "outlines" it
 * into a fake helper stub appended at the end:
 *
 * Original:     call .outlined_stub_N   ; [T25]
 *               ...
 * .outlined_stub_N:
 *   <original instruction>
 *   ret
 *
 * The VM skips OUTLINE_CALL (marks as dead for execution purposes)
 * and the real computation is unchanged because we leave the
 * original instruction in place and only add the fake call.
 * ============================================================ */
void cxs_outline_functions(cxs_engine_t *e) {
    int count = 0;

    for (int i = 4; i < e->num_insns - 8 && count < 3; i++) {
        insn_t *ins = &e->insns[i];
        if (ins->is_dead || ins->is_junk || ins->is_stub || ins->is_outlined ||
            ins->is_antiana || ins->is_virt || ins->is_loop)
            continue;
        if (ins->type != INSN_ADD && ins->type != INSN_SUB &&
            ins->type != INSN_NOP)
            continue;

        char lbl[CXS_LABEL_LEN];
        snprintf(lbl, sizeof(lbl), ".outlined_stub_%d", e->outline_serial++);

        /* Insert fake CALL before the instruction */
        insn_t ocall = blank_insn();
        ocall.type       = INSN_OUTLINE_CALL;
        ocall.is_outlined = 1;
        ocall.is_dead    = 1;   /* VM skips */
        ocall.block_id   = ins->block_id;
        ocall.ops[0]     = op_lbl(lbl);
        ocall.num_ops    = 1;
        if (insn_insert_before(e, i, &ocall) < 0) break;
        i++;  /* skip past call */

        /* Append stub at end: LABEL + copy of insn + RET */
        insn_t stub_lbl = blank_insn();
        stub_lbl.type       = INSN_LABEL;
        stub_lbl.is_outlined = 1;
        stub_lbl.is_dead    = 1;
        stub_lbl.block_id   = -1;
        snprintf(stub_lbl.label, CXS_LABEL_LEN, "%s", lbl);
        stub_lbl.has_label  = 1;
        if (insn_append(e, &stub_lbl) < 0) break;

        insn_t stub_body = *ins;
        stub_body.is_outlined = 1;
        stub_body.is_dead     = 1;
        if (insn_append(e, &stub_body) < 0) break;

        insn_t stub_ret = blank_insn();
        stub_ret.type       = INSN_OUTLINE_RET;
        stub_ret.is_outlined = 1;
        stub_ret.is_dead    = 1;
        stub_ret.block_id   = -1;
        if (insn_append(e, &stub_ret) < 0) break;

        if (e->outline_len < CXS_MAX_OUTLINE) {
            snprintf(e->outlines[e->outline_len].label,
                     CXS_LABEL_LEN, "%s", lbl);
            e->outlines[e->outline_len].used = 1;
            e->outline_len++;
        }

        printf("    [T25] Outlined stub '%s' at insn[%d]\n", lbl, i);
        count++;
    }

    e->stats.outlined_stubs += count;
    printf("    [T25] %d outline stub(s) created.\n", count);
}

/* ============================================================
 * T26 — Bitfield Extraction Noise
 *
 * Inserts sequences of bit-manipulation instructions on scratch
 * registers that look like bitfield extraction but produce
 * known-zero results:
 *
 *   XOR  sc, sc       ; sc = 0
 *   ROR  sc, 7        ; rotate 0 = still 0
 *   BSWAP sc          ; byte-swap 0 = still 0
 *   AND  sc, mask     ; mask 0 = 0
 *
 * Triggers false pattern recognition for bitfield extract
 * heuristics in decompilers (Hex-Rays, Ghidra).
 * ============================================================ */
void cxs_insert_bitfield_noise(cxs_engine_t *e) {
    int count = 0;

    for (int i = 3; i < e->num_insns - 6 && count < 4; i++) {
        insn_t *ins = &e->insns[i];
        if (!ins->is_junk && ins->type != INSN_NOP) continue;
        if (ins->is_dead || ins->is_outlined) continue;

        reg_id_t sc  = SCRATCH[t2_range(e, 0, NSCRATCH)];
        int      rot = t2_range(e, 1, 32);
        int64_t  msk = (int64_t)(t2_rand(e) & 0xFF);
        int      blk = ins->block_id;

        insn_t seq[4];
        for (int c = 0; c < 4; c++) {
            seq[c] = blank_insn();
            seq[c].is_bitfield = 1;
            seq[c].is_junk     = 1;
            seq[c].block_id    = blk;
        }

        seq[0].type = INSN_XOR;   seq[0].ops[0]=op_reg(sc); seq[0].ops[1]=op_reg(sc); seq[0].num_ops=2;
        seq[1].type = INSN_ROR;   seq[1].ops[0]=op_reg(sc); seq[1].ops[1]=op_imm(rot); seq[1].num_ops=2;
        seq[2].type = INSN_BSWAP; seq[2].ops[0]=op_reg(sc); seq[2].num_ops=1;
        seq[3].type = INSN_AND;   seq[3].ops[0]=op_reg(sc); seq[3].ops[1]=op_imm(msk); seq[3].num_ops=2;

        int inserted = 0;
        for (int c = 0; c < 4; c++) {
            if (insn_insert_before(e, i + inserted, &seq[c]) < 0) break;
            inserted++;
        }
        i += inserted;

        printf("    [T26] Bitfield seq at [%d]: %s ROR %d, BSWAP, AND 0x%"PRIx64"\n",
               i - inserted, cxs_reg_name(sc), rot, (uint64_t)msk);
        count++;
    }

    e->stats.bitfield_seqs += count;
    printf("    [T26] %d bitfield noise sequence(s) inserted.\n", count);
}

/* ============================================================
 * T27 — Checksum Guards
 *
 * Inserts a rolling XOR accumulator guard:
 *   XOR  guard, K      ; accumulate key K
 *   CMP  guard, 0
 *   JNE  .chk_ok_N    ; always taken (K≠0 so guard≠0)
 *   HLT / CALL bad     ; unreachable "tamper detected"
 * .chk_ok_N:
 *
 * This mimics a self-integrity check. Static analysis will
 * trace the guard register and spend time proving it's always
 * non-zero (it is, by construction).
 * ============================================================ */
void cxs_insert_checksum_guards(cxs_engine_t *e) {
    int count = 0;

    for (int i = 6; i < e->num_insns - 8 && count < 3; i++) {
        insn_t *ins = &e->insns[i];
        if (ins->is_dead || ins->is_junk || ins->is_stub || ins->is_chksum) continue;
        if (ins->type != INSN_NOP && !ins->is_junk) continue;

        reg_id_t guard = REG_R9;   /* dedicated guard register */
        int64_t  key   = (int64_t)(t2_rand(e) & 0xFFFFF) + 0x100;
        int      blk   = ins->block_id;

        char lbl_ok[CXS_LABEL_LEN];
        snprintf(lbl_ok, sizeof(lbl_ok), ".chk_ok_%d", e->chksum_serial++);

        insn_t seq[5];
        for (int c = 0; c < 5; c++) {
            seq[c] = blank_insn();
            seq[c].is_chksum = 1;
            seq[c].block_id  = blk;
        }

        /* XOR guard, key */
        seq[0].type = INSN_CHKSUM;
        seq[0].ops[0]=op_reg(guard); seq[0].ops[1]=op_imm(key); seq[0].num_ops=2;

        /* CMP guard, 0 */
        seq[1].type = INSN_CMP;
        seq[1].ops[0]=op_reg(guard); seq[1].ops[1]=op_imm(0); seq[1].num_ops=2;

        /* JNE .chk_ok */
        seq[2].type = INSN_JNE;
        seq[2].ops[0]=op_lbl(lbl_ok); seq[2].num_ops=1;

        /* "Tamper detected": XOR rax, rax  (unreachable but scary-looking) */
        seq[3].type = INSN_XOR;
        seq[3].is_dead = 1;
        seq[3].ops[0]=op_reg(REG_RAX); seq[3].ops[1]=op_reg(REG_RAX); seq[3].num_ops=2;

        /* .chk_ok NOP */
        seq[4].type = INSN_NOP;
        snprintf(seq[4].label, CXS_LABEL_LEN, "%s", lbl_ok);
        seq[4].has_label = 1;

        int inserted = 0;
        for (int c = 0; c < 5; c++) {
            if (insn_insert_before(e, i + inserted, &seq[c]) < 0) break;
            inserted++;
        }
        i += inserted;

        printf("    [T27] Checksum guard at [%d]: key=0x%"PRIx64" label=%s\n",
               i - inserted, (uint64_t)key, lbl_ok);
        count++;
    }

    e->stats.chksum_guards += count;
    printf("    [T27] %d checksum guard(s) inserted.\n", count);
}

/* ============================================================
 * T28 — Predicated Move Obfuscation (CMOV ladder)
 *
 * Replaces plain MOV dst, imm instructions with a CMOV ladder:
 *   XOR  dst, dst          ; dst = 0
 *   CMP  sc, sc            ; flags: ZF=1 (always)
 *   MOV  tmp, imm
 *   CMOVE dst, tmp         ; conditional move: dst=tmp if ZF=1 (always true)
 *
 * This replaces a trivially trackable data-flow with a
 * predicated assignment that requires flag analysis to resolve.
 * ============================================================ */
void cxs_obfuscate_moves(cxs_engine_t *e) {
    int count = 0;

    for (int i = 0; i < e->num_insns - 4 && count < 4; i++) {
        insn_t *ins = &e->insns[i];
        /* Find a plain MOV dst_reg, imm (non-decorated) */
        if (ins->type != INSN_MOV) continue;
        if (ins->is_junk || ins->is_dead || ins->is_stub || ins->is_cmov ||
            ins->is_encoded || ins->is_dataenc || ins->is_flat ||
            ins->is_alias || ins->is_virt || ins->is_bogus)
            continue;
        if (ins->num_ops != 2) continue;
        if (ins->ops[0].type != OP_REG) continue;
        if (ins->ops[1].type != OP_IMM) continue;

        int64_t  imm = ins->ops[1].imm;
        reg_id_t dst = ins->ops[0].reg;
        reg_id_t tmp = (dst == REG_R10) ? REG_R11 : REG_R10;
        reg_id_t sc  = REG_R12;
        int      blk = ins->block_id;

        /* Build CMOV ladder: 4 instructions replace original MOV */
        insn_t seq[4];
        for (int c = 0; c < 4; c++) {
            seq[c] = blank_insn();
            seq[c].is_cmov   = 1;
            seq[c].block_id  = blk;
        }

        /* XOR dst, dst */
        seq[0].type = INSN_XOR;
        seq[0].ops[0]=op_reg(dst); seq[0].ops[1]=op_reg(dst); seq[0].num_ops=2;

        /* CMP sc, sc  (sets ZF=1) */
        seq[1].type = INSN_CMP;
        seq[1].ops[0]=op_reg(sc); seq[1].ops[1]=op_reg(sc); seq[1].num_ops=2;

        /* MOV tmp, imm */
        seq[2].type = INSN_MOV;
        seq[2].ops[0]=op_reg(tmp); seq[2].ops[1]=op_imm(imm); seq[2].num_ops=2;

        /* CMOVE dst, tmp  (JE semantics: ZF=1 always → always executes) */
        /* Represent as: JE-conditioned MOV  — we emit this as a CMOV_INSN */
        seq[3].type = INSN_MOV;  /* VM treats as plain MOV; emitter outputs cmove */
        seq[3].is_cmov = 1;
        seq[3].ops[0]=op_reg(dst); seq[3].ops[1]=op_reg(tmp); seq[3].num_ops=2;

        /* Replace original MOV with ladder: remove original, insert 4 */
        /* Mark original as cmov so it's distinguishable */
        e->insns[i].is_cmov = 1;
        e->insns[i].type    = INSN_XOR;  /* first step */
        e->insns[i].ops[0]  = op_reg(dst);
        e->insns[i].ops[1]  = op_reg(dst);
        e->insns[i].num_ops = 2;

        /* Insert remaining 3 after it */
        for (int c = 1; c <= 3; c++) {
            if (insn_insert_before(e, i + c, &seq[c]) < 0) goto cmov_done;
        }
        i += 3;

        printf("    [T28] CMOV ladder for MOV %s, %"PRId64"\n",
               cxs_reg_name(dst), imm);
        count++;
    }

cmov_done:
    e->stats.cmov_chains += count;
    printf("    [T28] %d predicated-move chain(s) inserted.\n", count);
}

/* ============================================================
 * T29 — Entropy Injection
 *
 * Inserts fake RDRAND or PRNG-seed sequences that load
 * "random" values into scratch registers. These values
 * are then discarded (overwritten or never used), but
 * force data-flow analysis to consider unbounded values.
 *
 *   RDRAND sc         ; sc = random (range unknown)
 *   AND    sc, 0xFF   ; narrow to byte range
 *   XOR    sc, sc     ; sc = 0  (discard)
 * ============================================================ */
void cxs_inject_entropy(cxs_engine_t *e) {
    int count = 0;

    for (int i = 8; i < e->num_insns - 6 && count < 4; i++) {
        insn_t *ins = &e->insns[i];
        if (!ins->is_junk && ins->type != INSN_NOP) continue;
        if (ins->is_dead || ins->is_entropy) continue;

        reg_id_t sc  = SCRATCH[t2_range(e, 0, NSCRATCH)];
        int      blk = ins->block_id;

        char lbl_retry[CXS_LABEL_LEN];
        snprintf(lbl_retry, sizeof(lbl_retry), ".rdrand_retry_%d", e->entropy_serial++);

        insn_t seq[3];
        for (int c = 0; c < 3; c++) {
            seq[c] = blank_insn();
            seq[c].is_entropy = 1;
            seq[c].is_junk    = 1;
            seq[c].block_id   = blk;
        }

        /* RDRAND sc */
        seq[0].type = INSN_RDRAND;
        seq[0].ops[0]=op_reg(sc); seq[0].num_ops=1;
        snprintf(seq[0].label, CXS_LABEL_LEN, "%s", lbl_retry);
        seq[0].has_label = 1;

        /* AND sc, 0xFF  (narrow range — looks like a legitimate use) */
        seq[1].type = INSN_AND;
        seq[1].ops[0]=op_reg(sc); seq[1].ops[1]=op_imm(0xFF); seq[1].num_ops=2;

        /* XOR sc, sc  (discard — so result register unaffected) */
        seq[2].type = INSN_XOR;
        seq[2].ops[0]=op_reg(sc); seq[2].ops[1]=op_reg(sc); seq[2].num_ops=2;

        int inserted = 0;
        for (int c = 0; c < 3; c++) {
            if (insn_insert_before(e, i + inserted, &seq[c]) < 0) break;
            inserted++;
        }
        i += inserted;

        printf("    [T29] Entropy injection at [%d]: RDRAND %s → discard\n",
               i - inserted, cxs_reg_name(sc));
        count++;
    }

    e->stats.entropy_insns += count;
    printf("    [T29] %d entropy injection sequence(s) inserted.\n", count);
}

/* ============================================================
 * T30 — Multi-Layer Key Schedule
 *
 * Inserts a multi-round key expansion stub at function entry.
 * Each round performs:
 *   XOR  key_reg, round_const
 *   ADD  key_reg, round_add
 *   ROR  key_reg, round_rot  (emitted as SHR in IR)
 *
 * The key_reg is REG_R8 (not used by the function).
 * After CXS_KEYSCH_ROUNDS rounds, key_reg is discarded (XOR'd to 0).
 * This produces a complex-looking key expansion that mimics
 * AES-like schedule code and completely defeats key-recovery
 * heuristics on the surrounding code.
 * ============================================================ */
void cxs_insert_key_schedule(cxs_engine_t *e) {

    if (e->keysched_done) {
        printf("    [T30] Already applied, skipping.\n");
        return;
    }

    reg_id_t kreg = REG_R8;        /* key register (never touches result) */
    int64_t  seed = (int64_t)(t2_rand(e) | 1);
    int      insert_pos = 0;
    int      count = 0;

    /* Skip past existing label/stub at position 0 */
    while (insert_pos < e->num_insns &&
           (e->insns[insert_pos].type == INSN_LABEL ||
            e->insns[insert_pos].is_stub ||
            e->insns[insert_pos].is_cfi))
        insert_pos++;

    /* Initial seed load */
    insn_t m_init = blank_insn();
    m_init.type       = INSN_KEYSCHED;
    m_init.is_keysched = 1;
    m_init.block_id   = insert_pos < e->num_insns ? e->insns[insert_pos].block_id : 0;
    m_init.ops[0]     = op_reg(kreg);
    m_init.ops[1]     = op_imm(seed & 0xFFFFFFFF);
    m_init.num_ops    = 2;
    snprintf(m_init.label, CXS_LABEL_LEN, ".keysched_start");
    m_init.has_label  = 1;
    if (insn_insert_before(e, insert_pos++, &m_init) < 0) goto ks_done;
    count++;

    for (int r = 0; r < CXS_KEYSCH_ROUNDS; r++) {
        int64_t rc = (int64_t)(t2_rand(e) & 0xFFFFFF) ^ (seed >> (r & 7));
        int64_t ra = (int64_t)(t2_rand(e) & 0x0FFF) + 1;
        int     rr = (int)(t2_rand(e) % 16) + 1;
        int     blk = (insert_pos < e->num_insns) ? e->insns[insert_pos].block_id : 0;

        insn_t ks_xor = blank_insn();
        ks_xor.type       = INSN_KEYSCHED;
        ks_xor.is_keysched = 1;
        ks_xor.block_id   = blk;
        ks_xor.ops[0]     = op_reg(kreg);
        ks_xor.ops[1]     = op_imm(rc);
        ks_xor.num_ops    = 2;
        if (insn_insert_before(e, insert_pos++, &ks_xor) < 0) goto ks_done;
        count++;

        insn_t ks_add = blank_insn();
        ks_add.type       = INSN_ADD;
        ks_add.is_keysched = 1;
        ks_add.block_id   = blk;
        ks_add.ops[0]     = op_reg(kreg);
        ks_add.ops[1]     = op_imm(ra);
        ks_add.num_ops    = 2;
        if (insn_insert_before(e, insert_pos++, &ks_add) < 0) goto ks_done;
        count++;

        insn_t ks_ror = blank_insn();
        ks_ror.type       = INSN_ROR;
        ks_ror.is_keysched = 1;
        ks_ror.block_id   = blk;
        ks_ror.ops[0]     = op_reg(kreg);
        ks_ror.ops[1]     = op_imm(rr);
        ks_ror.num_ops    = 2;
        if (insn_insert_before(e, insert_pos++, &ks_ror) < 0) goto ks_done;
        count++;

        printf("    [T30] Round %2d: XOR 0x%"PRIx64"  ADD %"PRId64"  ROR %d\n",
               r, (uint64_t)rc, ra, rr);
        seed = (int64_t)((uint64_t)seed * 0x6c62272e07bb0142ULL + rc);
    }

    /* Final: discard key register (XOR kreg, kreg) */
    {
        insn_t ks_clr = blank_insn();
        ks_clr.type       = INSN_XOR;
        ks_clr.is_keysched = 1;
        ks_clr.block_id   = (insert_pos < e->num_insns) ? e->insns[insert_pos].block_id : 0;
        ks_clr.ops[0]     = op_reg(kreg);
        ks_clr.ops[1]     = op_reg(kreg);
        ks_clr.num_ops    = 2;
        snprintf(ks_clr.label, CXS_LABEL_LEN, ".keysched_end");
        ks_clr.has_label  = 1;
        if (insn_insert_before(e, insert_pos, &ks_clr) < 0) goto ks_done;
        count++;
    }

ks_done:
    e->keysched_done = 1;
    e->stats.keysched_rounds += CXS_KEYSCH_ROUNDS;
    printf("    [T30] %d key-schedule instruction(s), %d rounds.\n",
           count, CXS_KEYSCH_ROUNDS);
}
