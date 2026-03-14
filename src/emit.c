/*
 * emit.c  —  CXS Assembly Emitter  v1.0
 * ============================================================
 * Converts the post-transform CXS IR into a real, assemblable
 * .S file (GAS AT&T for x86-64, GAS for AArch64).
 *
 * Design rules:
 *  1. Only real / non-decoration instructions are emitted.
 *     Junk, opaque, overlap, stub, dead and anti-analysis
 *     instructions are each handled individually — some emit
 *     as real ASM (junk, opaque branches, stubs), others are
 *     suppressed (overlap byte markers, DEAD_CALL refs).
 *  2. Every emitted instruction gets an inline ; comment
 *     identifying which transform produced it.
 *  3. The output is self-contained: .section .text, .global,
 *     and a trailing .size directive are all included.
 *  4. The emitter is purely a printer — it never mutates the
 *     engine state.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cxs.h"
#include "cxs_emit.h"

/* ============================================================
 * Target detection helpers
 * ============================================================ */

static cxs_emit_target_t resolve_target(cxs_emit_target_t req) {
    if (req != CXS_EMIT_NATIVE) return req;
#if defined(CXS_ARCH_ARM64)
    return CXS_EMIT_ARM64;
#else
    return CXS_EMIT_X86_64;
#endif
}

const char *cxs_emit_default_filename(cxs_emit_target_t target) {
    switch (target) {
        case CXS_EMIT_ARM64:   return "obfuscated_arm64.S";
        default:               return "obfuscated_x86_64.S";
    }
}

void cxs_emit_opts_default(cxs_emit_opts_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->target     = CXS_EMIT_NATIVE;
    opts->outfile    = NULL;
    opts->fn_name    = "cxs_fn";
    opts->annotate   = 1;
    opts->show_stats = 1;
}

/* ============================================================
 * Register name tables
 * ============================================================ */

static const char *x64_regs[REG_COUNT] = {
    "rax","rbx","rcx","rdx","rsi","rdi","rsp","rbp",
    "r8","r9","r10","r11","r12","r13","r14","r15"
};
static const char *arm64_regs[REG_COUNT] = {
    "x0","x1","x2","x3","x4","x5","sp","fp",
    "x8","x9","x10","x11","x12","x13","x14","x15"
};

/* Alias for 32-bit sub-registers (for CPUID output on x86-64) */
static const char *x64_regs32[REG_COUNT] __attribute__((unused)) = {
    "eax","ebx","ecx","edx","esi","edi","esp","ebp",
    "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"
};

/* ============================================================
 * Operand renderer  (writes to caller-supplied buffer)
 *
 * x86-64 GAS AT&T:
 *   register  → %rax
 *   immediate → $5
 *   memory    → 8(%rsp)
 *   label     → .label
 *
 * AArch64:
 *   register  → x0
 *   immediate → #5
 *   memory    → [x0, #8]
 *   label     → .label
 * ============================================================ */

static int render_op(char *buf, int sz,
                     const operand_t *op,
                     cxs_emit_target_t tgt)
{
    int n = 0;
    switch (op->type) {
        case OP_REG:
            if (tgt == CXS_EMIT_X86_64)
                n = snprintf(buf, sz, "%%%s", x64_regs[op->reg]);
            else
                n = snprintf(buf, sz, "%s",   arm64_regs[op->reg]);
            break;
        case OP_IMM:
            if (tgt == CXS_EMIT_X86_64)
                n = snprintf(buf, sz, "$%"PRId64, op->imm);
            else
                n = snprintf(buf, sz, "#%"PRId64, op->imm);
            break;
        case OP_MEM:
            if (tgt == CXS_EMIT_X86_64)
                n = snprintf(buf, sz, "%"PRId64"(%%%s)",
                             op->mem.disp, x64_regs[op->mem.base]);
            else
                n = snprintf(buf, sz, "[%s, #%"PRId64"]",
                             arm64_regs[op->mem.base], op->mem.disp);
            break;
        case OP_LABEL:
            n = snprintf(buf, sz, "%s", op->label);
            break;
        case OP_JTAB:
            n = snprintf(buf, sz, "/* jtab[%d] */", op->jtab_slot);
            break;
        default:
            n = snprintf(buf, sz, "???");
            break;
    }
    return (n < 0) ? 0 : n;
}

/* Render a full operand list:  op0, op1  (AT&T: src,dst order for 2-ops) */
static void render_ops(char *buf, int sz,
                       const insn_t *ins,
                       cxs_emit_target_t tgt)
{
    buf[0] = '\0';
    if (ins->num_ops == 0) return;

    char tmp[64];
    int  off = 0;

    if (tgt == CXS_EMIT_X86_64 && ins->num_ops == 2) {
        /* AT&T: src, dst */
        off += render_op(buf+off, sz-off, &ins->ops[1], tgt);
        if (off < sz-2) { buf[off++]=','; buf[off++]=' '; }
        off += render_op(buf+off, sz-off, &ins->ops[0], tgt);
    } else {
        /* Intel / AArch64: dst, src */
        for (int i = 0; i < ins->num_ops && off < sz-3; i++) {
            if (i) { buf[off++]=','; buf[off++]=' '; }
            off += render_op(tmp, sizeof(tmp), &ins->ops[i], tgt);
            int cp = (int)strlen(tmp);
            if (off+cp < sz-1) { memcpy(buf+off, tmp, cp); off+=cp; }
        }
    }
    buf[off] = '\0';
}

/* ============================================================
 * Transform annotation comment
 * ============================================================ */

static const char *transform_comment(const insn_t *ins) {
    if (ins->is_antiana)  return "// [T15] anti-analysis";
    if (ins->is_stub)     return "// [T14] decrypt stub";
    if (ins->is_stack)    return "// [T13] stack noise";
    if (ins->is_split)    return "// [T12] data-flow split";
    if (ins->is_subst)    return "// [T11] opcode substitution";
    if (ins->is_dead)     return "// [T10] dead code";
    if (ins->is_indirect) return "// [T9]  indirect jump";
    if (ins->is_flat)     return "// [T7]  CFF dispatch";
    if (ins->is_encoded)  return "// [T8]  encoded constant";
    if (ins->is_opaque)   return "// [T4]  opaque predicate";
    if (ins->is_junk)     return "// [T2]  junk";
    return "";  /* real instruction — no comment */
}

/* ============================================================
 * x86-64 GAS AT&T instruction emitter
 *
 * Maps CXS IR insn_type_t → GAS mnemonic + operands.
 * AT&T peculiarities handled:
 *   - Operand reversal (src, dst)
 *   - Register %prefix
 *   - Immediate $prefix
 *   - IMUL has Intel-style (dst, src) even in AT&T
 *   - IDIV / DIV take a single divisor operand
 *   - SHL/SHR shift count in %cl or $imm
 *   - IJMP → jmp *%r14
 *   - RDTSC → rdtsc; mov %eax,%r10d (lower 32)
 *   - CPUID → mov $1,%eax; cpuid
 * ============================================================ */

static void emit_x64_insn(FILE *fp, const insn_t *ins, int annotate) {
    char ops[128] = "";
    render_ops(ops, sizeof(ops), ins, CXS_EMIT_X86_64);
    const char *ann = annotate ? transform_comment(ins) : "";

/* Shorthand: print padded mnemonic + operands + comment */
#define EMIT(mne, operands) \
    fprintf(fp, "    %-8s %-28s %s\n", (mne), (operands), (ann))

    switch (ins->type) {
        /* ── No-op ── */
        case INSN_NOP:       EMIT("nop",   ""); break;

        /* ── Data movement ── */
        case INSN_MOV:
        case INSN_STATE_MOV: EMIT("movq",  ops); break;
        case INSN_LEA:       EMIT("leaq",  ops); break;
        case INSN_XCHG:      EMIT("xchgq", ops); break;
        case INSN_PUSH:      EMIT("pushq", ops); break;
        case INSN_POP:       EMIT("popq",  ops); break;

        /* ── Arithmetic ── */
        case INSN_ADD:       EMIT("addq",  ops); break;
        case INSN_SUB:       EMIT("subq",  ops); break;
        case INSN_INC:       EMIT("incq",  ops); break;
        case INSN_DEC:       EMIT("decq",  ops); break;
        case INSN_NEG:       EMIT("negq",  ops); break;
        case INSN_MUL:       EMIT("mulq",  ops); break;
        case INSN_DIV:       EMIT("divq",  ops); break;
        case INSN_IDIV:      EMIT("idivq", ops); break;
        case INSN_IMUL: {
            /* AT&T IMUL dst,src is non-standard; use Intel form via 2-op */
            if (ins->num_ops == 2) {
                /* imulq $imm, %reg  or  imulq %reg, %reg */
                char d[32], s[32];
                render_op(d, sizeof(d), &ins->ops[0], CXS_EMIT_X86_64);
                render_op(s, sizeof(s), &ins->ops[1], CXS_EMIT_X86_64);
                char combo[80]; snprintf(combo, sizeof(combo), "%s, %s", s, d);
                EMIT("imulq", combo);
            } else {
                EMIT("imulq", ops);
            }
            break;
        }

        /* ── Bitwise ── */
        case INSN_XOR:       EMIT("xorq",  ops); break;
        case INSN_AND:       EMIT("andq",  ops); break;
        case INSN_OR:        EMIT("orq",   ops); break;
        case INSN_NOT:       EMIT("notq",  ops); break;
        case INSN_SHL: {
            /* SHL rax, imm  →  shlq $imm, %rax */
            if (ins->num_ops == 2) {
                char d[32], s[32];
                render_op(d, sizeof(d), &ins->ops[0], CXS_EMIT_X86_64);
                render_op(s, sizeof(s), &ins->ops[1], CXS_EMIT_X86_64);
                char combo[80]; snprintf(combo, sizeof(combo), "%s, %s", s, d);
                EMIT("shlq", combo);
            } else { EMIT("shlq", ops); }
            break;
        }
        case INSN_SHR: {
            if (ins->num_ops == 2) {
                char d[32], s[32];
                render_op(d, sizeof(d), &ins->ops[0], CXS_EMIT_X86_64);
                render_op(s, sizeof(s), &ins->ops[1], CXS_EMIT_X86_64);
                char combo[80]; snprintf(combo, sizeof(combo), "%s, %s", s, d);
                EMIT("shrq", combo);
            } else { EMIT("shrq", ops); }
            break;
        }

        /* ── Comparison ── */
        case INSN_CMP:
        case INSN_STATE_CMP: EMIT("cmpq",  ops); break;
        case INSN_TEST:      EMIT("testq", ops); break;

        /* ── Control flow ── */
        case INSN_JMP: {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("jmp", ins->ops[0].label);
            else
                EMIT("jmp", ops);
            break;
        }
        case INSN_JE:  {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("je",  ins->ops[0].label);
            else
                EMIT("je",  ops);
            break;
        }
        case INSN_JNE: {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("jne", ins->ops[0].label);
            else
                EMIT("jne", ops);
            break;
        }
        case INSN_JL:  {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("jl",  ins->ops[0].label);
            else EMIT("jl",  ops);
            break;
        }
        case INSN_JG:  {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("jg",  ins->ops[0].label);
            else EMIT("jg",  ops);
            break;
        }
        case INSN_JLE: {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("jle", ins->ops[0].label);
            else EMIT("jle", ops);
            break;
        }
        case INSN_JGE: {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("jge", ins->ops[0].label);
            else EMIT("jge", ops);
            break;
        }
        case INSN_CALL:      EMIT("call",  ops); break;
        case INSN_RET:       EMIT("ret",   "");  break;

        /* ── T9: Indirect jump ── */
        case INSN_IJMP: {
            char buf[32];
            snprintf(buf, sizeof(buf), "*%%%s", x64_regs[REG_ITGT]);
            EMIT("jmp", buf);
            break;
        }

        /* ── T7: CFF state instructions ── */
        /* Already emitted as cmpq / movq above via STATE_CMP/STATE_MOV */

        /* ── T13: Fake stack noise (emitted as comments to avoid real SP changes) ── */
        case INSN_FAKE_PUSH:
            fprintf(fp, "    // [T13] fake_push %s (stack noise)\n",
                    (ins->num_ops > 0 && ins->ops[0].type == OP_REG)
                    ? x64_regs[ins->ops[0].reg] : "?");
            break;
        case INSN_FAKE_POP:
            fprintf(fp, "    // [T13] fake_pop  %s (stack noise)\n",
                    (ins->num_ops > 0 && ins->ops[0].type == OP_REG)
                    ? x64_regs[ins->ops[0].reg] : "?");
            break;

        /* ── T14: XOR decrypt stub ── */
        case INSN_XOR_STUB:  EMIT("xorq",  ops); break;

        /* ── T15: Anti-analysis ── */
        case INSN_CPUID:
            /* Standard CPUID idiom: load leaf → cpuid */
            fprintf(fp, "    %-8s %-28s %s\n", "movl",
                    "$1, %eax", annotate ? "// [T15] CPUID leaf 1" : "");
            fprintf(fp, "    %-8s %-28s %s\n", "cpuid",
                    "", annotate ? "// [T15] anti-analysis trap" : "");
            break;
        case INSN_RDTSC:
            fprintf(fp, "    %-8s %-28s %s\n", "rdtsc",
                    "", annotate ? "// [T15] timing check" : "");
            /* Combine eax:edx into r10 for timing delta check */
            fprintf(fp, "    %-8s %-28s %s\n", "shlq",
                    "$32, %rdx", annotate ? "// [T15] combine TSC high" : "");
            fprintf(fp, "    %-8s %-28s %s\n", "orq",
                    "%rdx, %rax", annotate ? "// [T15] combine TSC low" : "");
            fprintf(fp, "    %-8s %-28s %s\n", "movq",
                    "%rax, %r10", annotate ? "// [T15] store TSC" : "");
            break;

        /* ── Decoration: suppress ── */
        case INSN_OVERLAP:
        case INSN_DEAD_CALL:
        case INSN_JUNK:
        case INSN_LABEL:
            /* handled by caller */
            break;

        default:
            fprintf(fp, "    /* unhandled insn type %d */\n", ins->type);
            break;
    }
#undef EMIT
}

/* ============================================================
 * AArch64 GAS instruction emitter
 *
 * Key rules:
 *   - Comments use // not ;
 *   - mov xN, #imm  only accepts 16-bit shifted immediates
 *     For any imm that doesn't fit, we emit movz/movk sequence
 *   - add/sub #imm  only accepts 0..4095 (12-bit)
 *     For larger constants we load into x15 first then use reg form
 *   - .cfi_* directives have NO trailing colon
 * ============================================================ */

/* Emit a 64-bit immediate into a register using movz+movk.
 * Always correct, works for any 64-bit value. */
static void emit_arm64_mov_imm(FILE *fp, const char *reg, int64_t v,
                                const char *ann) {
    uint64_t u = (uint64_t)v;
    uint16_t h0 = (u >>  0) & 0xFFFF;
    uint16_t h1 = (u >> 16) & 0xFFFF;
    uint16_t h2 = (u >> 32) & 0xFFFF;
    uint16_t h3 = (u >> 48) & 0xFFFF;

    /* Count non-zero halfwords */
    int nz = (h0!=0) + (h1!=0) + (h2!=0) + (h3!=0);

    if (nz == 0) {
        /* mov reg, #0 is always valid */
        fprintf(fp, "    %-8s %s, #0%s%s\n", "mov", reg,
                ann[0] ? "                         " : "", ann);
        return;
    }

    /* Prefer movn (move NOT) when the upper bits are all-ones — shorter */
    /* But for simplicity always use movz + movk: correct and portable */
    int first = 1;
    uint16_t hws[4] = {h0, h1, h2, h3};
    int      shifts[4] = {0, 16, 32, 48};
    for (int i = 0; i < 4; i++) {
        if (hws[i] == 0 && !first) continue;  /* skip zero chunks after first */
        if (first) {
            fprintf(fp, "    %-8s %s, #0x%x, lsl #%d%s%s\n",
                    "movz", reg, hws[i], shifts[i],
                    ann[0] ? "          " : "", first ? ann : "");
            first = 0;
        } else {
            fprintf(fp, "    %-8s %s, #0x%x, lsl #%d\n",
                    "movk", reg, hws[i], shifts[i]);
        }
    }
}

/* Emit add/sub with a possibly-large immediate via x15 scratch */
static void emit_arm64_addsub_imm(FILE *fp, const char *mne,
                                   const char *dst, int64_t imm,
                                   const char *ann) {
    /* add/sub accept 0..4095 shifted by 0 or 12 */
    uint64_t u = (imm < 0) ? (uint64_t)(-imm) : (uint64_t)imm;
    /* If negative imm on add → flip to sub and vice-versa */
    const char *real_mne = mne;
    uint64_t    real_u   = u;
    if (imm < 0) {
        real_mne = (strcmp(mne,"add")==0) ? "sub" : "add";
        real_u   = (uint64_t)(-imm);
    }
    if (real_u <= 4095) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s, %s, #%"PRIu64, dst, dst, real_u);
        fprintf(fp, "    %-8s %-32s %s\n", real_mne, buf, ann);
    } else {
        /* Load into x15, then use register form */
        emit_arm64_mov_imm(fp, "x15", (int64_t)real_u, "");
        char buf[64];
        snprintf(buf, sizeof(buf), "%s, %s, x15", dst, dst);
        fprintf(fp, "    %-8s %-32s %s\n", real_mne, buf, ann);
    }
}

/* Emit an immediate into scratch register x15, for use as operand */
static void emit_arm64_scratch_imm(FILE *fp, int64_t val) {
    emit_arm64_mov_imm(fp, "x15", val, "// [emit] imm\xe2\x86\x92scratch");
}

static void emit_arm64_insn(FILE *fp, const insn_t *ins, int annotate) {
    const char *ann = annotate ? transform_comment(ins) : "";

/* AArch64 comment char is // not ; */
#define EMIT(mne, operands) \
    fprintf(fp, "    %-8s %-32s %s\n", (mne), (operands), (ann))
#define REG(r) arm64_regs[(r)]

    switch (ins->type) {
        case INSN_NOP: EMIT("nop", ""); break;

        /* ── Data movement ── */
        case INSN_MOV:
        case INSN_STATE_MOV: {
            if (ins->num_ops == 2) {
                if (ins->ops[1].type == OP_IMM) {
                    emit_arm64_mov_imm(fp, REG(ins->ops[0].reg),
                                       ins->ops[1].imm, ann);
                } else if (ins->ops[1].type == OP_REG) {
                    char buf[48];
                    snprintf(buf, sizeof(buf), "%s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[1].reg));
                    EMIT("mov", buf);
                }
            }
            break;
        }
        case INSN_LEA: {
            if (ins->num_ops == 2 && ins->ops[1].type == OP_MEM) {
                char buf[80];
                snprintf(buf, sizeof(buf), "%s, [%s, #%"PRId64"]",
                         REG(ins->ops[0].reg),
                         REG(ins->ops[1].mem.base),
                         ins->ops[1].mem.disp);
                EMIT("add", buf);
            }
            break;
        }
        case INSN_XCHG: {
            if (ins->num_ops == 2 && ins->ops[0].type == OP_REG
                                  && ins->ops[1].type == OP_REG) {
                fprintf(fp, "    eor     %s, %s, %s\n",
                        REG(ins->ops[0].reg), REG(ins->ops[0].reg), REG(ins->ops[1].reg));
                fprintf(fp, "    eor     %s, %s, %s\n",
                        REG(ins->ops[1].reg), REG(ins->ops[1].reg), REG(ins->ops[0].reg));
                fprintf(fp, "    eor     %s, %s, %s\n",
                        REG(ins->ops[0].reg), REG(ins->ops[0].reg), REG(ins->ops[1].reg));
            }
            break;
        }
        case INSN_PUSH: {
            if (ins->num_ops >= 1 && ins->ops[0].type == OP_REG) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s, [sp, #-16]!", REG(ins->ops[0].reg));
                EMIT("str", buf);
            }
            break;
        }
        case INSN_POP: {
            if (ins->num_ops >= 1 && ins->ops[0].type == OP_REG) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s, [sp], #16", REG(ins->ops[0].reg));
                EMIT("ldr", buf);
            }
            break;
        }

        /* ── T13 fake push/pop: emit as comments ── */
        case INSN_FAKE_PUSH:
            fprintf(fp, "    // [T13] fake_push %s (stack noise)\n",
                    (ins->num_ops > 0 && ins->ops[0].type == OP_REG)
                    ? REG(ins->ops[0].reg) : "?");
            break;
        case INSN_FAKE_POP:
            fprintf(fp, "    // [T13] fake_pop  %s (stack noise)\n",
                    (ins->num_ops > 0 && ins->ops[0].type == OP_REG)
                    ? REG(ins->ops[0].reg) : "?");
            break;

        /* ── Arithmetic ── */
        case INSN_ADD: {
            if (ins->num_ops == 2) {
                if (ins->ops[1].type == OP_IMM)
                    emit_arm64_addsub_imm(fp, "add", REG(ins->ops[0].reg),
                                          ins->ops[1].imm, ann);
                else {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                    EMIT("add", buf);
                }
            }
            break;
        }
        case INSN_SUB: {
            if (ins->num_ops == 2) {
                if (ins->ops[1].type == OP_IMM)
                    emit_arm64_addsub_imm(fp, "sub", REG(ins->ops[0].reg),
                                          ins->ops[1].imm, ann);
                else {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                    EMIT("sub", buf);
                }
            }
            break;
        }
        case INSN_MUL:
        case INSN_IMUL: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM) {
                    emit_arm64_scratch_imm(fp, ins->ops[1].imm);
                    snprintf(buf, sizeof(buf), "%s, %s, x15",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                } else {
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                }
                EMIT("mul", buf);
            }
            break;
        }
        case INSN_INC: {
            if (ins->num_ops == 1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s, %s, #1",
                         REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                EMIT("add", buf);
            }
            break;
        }
        case INSN_DEC: {
            if (ins->num_ops == 1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s, %s, #1",
                         REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                EMIT("sub", buf);
            }
            break;
        }
        case INSN_NEG: {
            if (ins->num_ops == 1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s, %s",
                         REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                EMIT("neg", buf);
            }
            break;
        }
        case INSN_DIV:
        case INSN_IDIV: {
            if (ins->num_ops >= 1) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%s, %s, %s",
                         REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                         (ins->num_ops >= 2 && ins->ops[1].type == OP_REG)
                         ? REG(ins->ops[1].reg) : "x1");
                EMIT((ins->type == INSN_IDIV) ? "sdiv" : "udiv", buf);
            }
            break;
        }

        /* ── Bitwise ── */
        case INSN_XOR:
        case INSN_XOR_STUB: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM) {
                    emit_arm64_scratch_imm(fp, ins->ops[1].imm);
                    snprintf(buf, sizeof(buf), "%s, %s, x15",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                } else {
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                }
                EMIT("eor", buf);
            }
            break;
        }
        case INSN_AND: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM) {
                    if (ins->ops[1].imm == -1) {
                        /* and r, -1 = NOP on AArch64 */
                        fprintf(fp, "    // and %s, -1 (nop)  %s\n",
                                REG(ins->ops[0].reg), ann);
                    } else {
                        emit_arm64_scratch_imm(fp, ins->ops[1].imm);
                        snprintf(buf, sizeof(buf), "%s, %s, x15",
                                 REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                        EMIT("and", buf);
                    }
                } else {
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                    EMIT("and", buf);
                }
            }
            break;
        }
        case INSN_OR: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM) {
                    emit_arm64_scratch_imm(fp, ins->ops[1].imm);
                    snprintf(buf, sizeof(buf), "%s, %s, x15",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                } else {
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                }
                EMIT("orr", buf);
            }
            break;
        }
        case INSN_NOT: {
            if (ins->num_ops == 1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s, %s",
                         REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                EMIT("mvn", buf);
            }
            break;
        }
        case INSN_SHL: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM)
                    snprintf(buf, sizeof(buf), "%s, %s, #%"PRId64,
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             ins->ops[1].imm);
                else
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                EMIT("lsl", buf);
            }
            break;
        }
        case INSN_SHR: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM)
                    snprintf(buf, sizeof(buf), "%s, %s, #%"PRId64,
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             ins->ops[1].imm);
                else
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                EMIT("lsr", buf);
            }
            break;
        }

        /* ── T26: ROR, BSWAP ── */
        case INSN_ROR: {
            if (ins->num_ops == 2) {
                char buf[64];
                /* AArch64: ror xd, xn, #imm  (alias of extr) */
                if (ins->ops[1].type == OP_IMM)
                    snprintf(buf, sizeof(buf), "%s, %s, #%"PRId64,
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             ins->ops[1].imm & 63);
                else
                    snprintf(buf, sizeof(buf), "%s, %s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[0].reg),
                             REG(ins->ops[1].reg));
                EMIT("ror", buf);
            }
            break;
        }
        case INSN_BSWAP: {
            if (ins->num_ops == 1) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%s, %s",
                         REG(ins->ops[0].reg), REG(ins->ops[0].reg));
                EMIT("rev", buf);  /* AArch64: rev = byte-reverse */
            }
            break;
        }

        /* ── Comparison ── */
        case INSN_CMP:
        case INSN_STATE_CMP: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM) {
                    /* cmp only takes 12-bit imm; load large values via x15 */
                    int64_t imm = ins->ops[1].imm;
                    if (imm >= 0 && imm <= 4095) {
                        snprintf(buf, sizeof(buf), "%s, #%"PRId64,
                                 REG(ins->ops[0].reg), imm);
                        EMIT("cmp", buf);
                    } else {
                        emit_arm64_scratch_imm(fp, imm);
                        snprintf(buf, sizeof(buf), "%s, x15",
                                 REG(ins->ops[0].reg));
                        EMIT("cmp", buf);
                    }
                } else {
                    snprintf(buf, sizeof(buf), "%s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[1].reg));
                    EMIT("cmp", buf);
                }
            }
            break;
        }
        case INSN_TEST: {
            if (ins->num_ops == 2) {
                char buf[80];
                if (ins->ops[1].type == OP_IMM) {
                    emit_arm64_scratch_imm(fp, ins->ops[1].imm);
                    snprintf(buf, sizeof(buf), "%s, x15",
                             REG(ins->ops[0].reg));
                    EMIT("tst", buf);
                } else {
                    snprintf(buf, sizeof(buf), "%s, %s",
                             REG(ins->ops[0].reg), REG(ins->ops[1].reg));
                    EMIT("tst", buf);
                }
            }
            break;
        }

        /* ── Control flow ── */
        case INSN_JMP: {
            if (ins->num_ops == 1 && ins->ops[0].type == OP_LABEL)
                EMIT("b", ins->ops[0].label);
            break;
        }
        case INSN_JE:  { if (ins->num_ops==1) EMIT("b.eq", ins->ops[0].label); break; }
        case INSN_JNE: { if (ins->num_ops==1) EMIT("b.ne", ins->ops[0].label); break; }
        case INSN_JL:  { if (ins->num_ops==1) EMIT("b.lt", ins->ops[0].label); break; }
        case INSN_JG:  { if (ins->num_ops==1) EMIT("b.gt", ins->ops[0].label); break; }
        case INSN_JLE: { if (ins->num_ops==1) EMIT("b.le", ins->ops[0].label); break; }
        case INSN_JGE: { if (ins->num_ops==1) EMIT("b.ge", ins->ops[0].label); break; }
        case INSN_CALL:{ if (ins->num_ops==1) EMIT("bl",   ins->ops[0].label); break; }
        case INSN_RET: EMIT("ret", ""); break;

        /* ── T9: Indirect jump ── */
        case INSN_IJMP: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%s", arm64_regs[REG_ITGT]);
            EMIT("br", buf);
            break;
        }

        /* ── T15: Anti-analysis ── */
        case INSN_CPUID:
            fprintf(fp, "    %-8s %-32s %s\n", "mrs",
                    "x10, MIDR_EL1",
                    annotate ? "// [T15] feature check" : "");
            break;
        case INSN_RDTSC:
            fprintf(fp, "    %-8s %-32s %s\n", "mrs",
                    "x10, CNTVCT_EL0",
                    annotate ? "// [T15] timing check" : "");
            break;

        /* ── T17: Bogus args: emit as mov (decorative) ── */
        case INSN_BOGUS_ARG: {
            if (ins->num_ops == 2 && ins->ops[1].type == OP_IMM)
                emit_arm64_mov_imm(fp, REG(ins->ops[0].reg),
                                   ins->ops[1].imm, ann);
            break;
        }

        /* ── T19: Virt enter/op/exit: emit as comments ── */
        case INSN_VIRT_ENTER:
        case INSN_VIRT_OP:
        case INSN_VIRT_EXIT:
            fprintf(fp, "    // [T19] virt %s  %s\n",
                    ins->type == INSN_VIRT_ENTER ? "enter" :
                    ins->type == INSN_VIRT_OP    ? "op"    : "exit",
                    ann);
            break;

        /* ── T21: Loop init/cmp: emit as real instructions ── */
        case INSN_LOOP_INIT: {
            /* MOV x8, 1  (loop counter) */
            emit_arm64_mov_imm(fp, "x8", 1, ann);
            break;
        }
        case INSN_LOOP_CMP: {
            /* SUBS x8, x8, #1 / B.NE label */
            if (ins->num_ops >= 1 && ins->ops[0].type == OP_LABEL) {
                fprintf(fp, "    %-8s %-32s %s\n", "subs", "x8, x8, #1", ann);
                fprintf(fp, "    %-8s %s\n", "b.ne", ins->ops[0].label);
            }
            break;
        }

        /* ── T22: CFI frame markers ── */
        case INSN_CFI_START:
            /* .cfi_def_cfa_offset labels are handled in the loop above;
             * here we only reach startproc (no label) or unexpected labels */
            fprintf(fp, "    .cfi_startproc\n");
            break;
        case INSN_CFI_END:
            fprintf(fp, "    .cfi_endproc\n");
            break;

        /* ── T25: Outline call: emit as bl to stub ── */
        case INSN_OUTLINE_CALL: {
            if (ins->num_ops >= 1 && ins->ops[0].type == OP_LABEL)
                EMIT("bl", ins->ops[0].label);
            break;
        }
        case INSN_OUTLINE_RET:
            EMIT("ret", "");
            break;

        /* ── T27: Checksum guard: emit as mov+cmp+b.ne ── */
        case INSN_CHKSUM: {
            if (ins->num_ops >= 2 && ins->ops[0].type == OP_REG
                                  && ins->ops[1].type == OP_IMM) {
                emit_arm64_mov_imm(fp, REG(ins->ops[0].reg),
                                   ins->ops[1].imm, ann);
            }
            break;
        }

        /* ── T29: Entropy / RDRAND ── */
        case INSN_RDRAND: {
            /* AArch64 has no RDRAND; emit RNDR if available, else MRS */
            if (ins->num_ops >= 1 && ins->ops[0].type == OP_REG) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%s, S3_3_c2_c4_0",
                         REG(ins->ops[0].reg));
                fprintf(fp, "    %-8s %-32s %s\n", "mrs", buf,
                        annotate ? "// [T29] entropy (RNDR)" : "");
            }
            break;
        }

        /* ── T30: Key schedule ── */
        case INSN_KEYSCHED: {
            /* Emit as mov x8, #imm */
            if (ins->num_ops >= 2 && ins->ops[1].type == OP_IMM)
                emit_arm64_mov_imm(fp, "x8", ins->ops[1].imm, ann);
            else if (ins->num_ops >= 1 && ins->ops[0].type == OP_REG)
                emit_arm64_mov_imm(fp, REG(ins->ops[0].reg), 0, ann);
            break;
        }

        /* ── Suppressed decoration ── */
        case INSN_OVERLAP:
        case INSN_DEAD_CALL:
        case INSN_JUNK:
        case INSN_LABEL:
            break;

        default:
            fprintf(fp, "    // unhandled insn type %d\n", ins->type);
            break;
    }

#undef EMIT
#undef REG
}


/* ============================================================
 * File-level prologue / epilogue writers
 * ============================================================ */

static void write_prologue_x64(FILE *fp, const char *fn,
                                const cxs_engine_t *e, int show_stats) {
    time_t t = time(NULL);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));

    fprintf(fp, "/*\n");
    fprintf(fp, " * Generated by CXS v4.0 (Code eXecution Scrambler)\n");
    fprintf(fp, " * Target    : x86-64  GAS AT&T syntax\n");
    fprintf(fp, " * Generated : %s\n", ts);
    if (e->has_source)
        fprintf(fp, " * Source    : %s\n", e->source_file);
    fprintf(fp, " *\n");
    if (show_stats) {
        fprintf(fp, " * Transform summary:\n");
        fprintf(fp, " *   T1  arith subs   : %d\n", e->stats.arith_subs);
        fprintf(fp, " *   T2  junk insns   : %d\n", e->stats.junk_injected);
        fprintf(fp, " *   T3  blocks reord : %d\n", e->stats.blocks_reordered);
        fprintf(fp, " *   T4  opaque preds : %d\n", e->stats.opaque_inserted);
        fprintf(fp, " *   T5  regs renamed : %d\n", e->stats.regs_renamed);
        fprintf(fp, " *   T6  overlaps     : %d\n", e->stats.overlap_inserted);
        fprintf(fp, " *   T7  CFF blocks   : %d\n", e->stats.blocks_flattened);
        fprintf(fp, " *   T8  consts enc   : %d\n", e->stats.consts_encoded);
        fprintf(fp, " *   T9  indirect jmp : %d\n", e->stats.jumps_indirected);
        fprintf(fp, " *   T10 dead blocks  : %d\n", e->stats.dead_blocks);
        fprintf(fp, " *   T11 opcode subs  : %d\n", e->stats.insns_substituted);
        fprintf(fp, " *   T12 var splits   : %d\n", e->stats.vars_split);
        fprintf(fp, " *   T13 stack noise  : %d\n", e->stats.stack_noise);
        fprintf(fp, " *   T14 stub insns   : %d\n", e->stats.stub_insns);
        fprintf(fp, " *   T15 antiana      : %d\n", e->stats.antiana_markers);
        fprintf(fp, " *   Total insns      : %d\n", e->num_insns);
        fprintf(fp, " *\n");
    }
    fprintf(fp, " * Assemble:\n");
    fprintf(fp, " *   cc -c -o out.o <this_file>\n");
    fprintf(fp, " */\n\n");

    fprintf(fp, "    .section .text\n");
    fprintf(fp, "    .global %s\n", fn);
    fprintf(fp, "    .type   %s, @function\n\n", fn);
    fprintf(fp, "%s:\n", fn);
}

static void write_epilogue_x64(FILE *fp, const char *fn) {
    fprintf(fp, "\n    .size %s, .-%s\n", fn, fn);
}

static void write_prologue_arm64(FILE *fp, const char *fn,
                                  const cxs_engine_t *e, int show_stats) {
    time_t t = time(NULL);
    char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));

    fprintf(fp, "/*\n");
    fprintf(fp, " * Generated by CXS v4.0 (Code eXecution Scrambler)\n");
    fprintf(fp, " * Target    : AArch64  GAS syntax\n");
    fprintf(fp, " * Generated : %s\n", ts);
    if (e->has_source)
        fprintf(fp, " * Source    : %s\n", e->source_file);
    fprintf(fp, " *\n");
    if (show_stats) {
        fprintf(fp, " * Transform summary:\n");
        fprintf(fp, " *   T1  arith subs   : %d\n", e->stats.arith_subs);
        fprintf(fp, " *   T2  junk insns   : %d\n", e->stats.junk_injected);
        fprintf(fp, " *   T3  blocks reord : %d\n", e->stats.blocks_reordered);
        fprintf(fp, " *   T4  opaque preds : %d\n", e->stats.opaque_inserted);
        fprintf(fp, " *   T5  regs renamed : %d\n", e->stats.regs_renamed);
        fprintf(fp, " *   T6  overlaps     : %d\n", e->stats.overlap_inserted);
        fprintf(fp, " *   T7  CFF blocks   : %d\n", e->stats.blocks_flattened);
        fprintf(fp, " *   T8  consts enc   : %d\n", e->stats.consts_encoded);
        fprintf(fp, " *   T9  indirect jmp : %d\n", e->stats.jumps_indirected);
        fprintf(fp, " *   T10 dead blocks  : %d\n", e->stats.dead_blocks);
        fprintf(fp, " *   T11 opcode subs  : %d\n", e->stats.insns_substituted);
        fprintf(fp, " *   T12 var splits   : %d\n", e->stats.vars_split);
        fprintf(fp, " *   T13 stack noise  : %d\n", e->stats.stack_noise);
        fprintf(fp, " *   T14 stub insns   : %d\n", e->stats.stub_insns);
        fprintf(fp, " *   T15 antiana      : %d\n", e->stats.antiana_markers);
        fprintf(fp, " *   Total insns      : %d\n", e->num_insns);
        fprintf(fp, " *\n");
    }
    fprintf(fp, " * Assemble:\n");
    fprintf(fp, " *   cc -c -o out.o <this_file>  (clang / GCC)\n");
    fprintf(fp, " */\n\n");

    fprintf(fp, "    .section .text\n");
    fprintf(fp, "    .global %s\n", fn);
    fprintf(fp, "    .type   %s, %%function\n\n", fn);
    fprintf(fp, "%s:\n", fn);
}

static void write_epilogue_arm64(FILE *fp, const char *fn) {
    fprintf(fp, "\n    .size %s, .-%s\n", fn, fn);
}

/* Jump table data section */
static void write_jtab(FILE *fp, const cxs_engine_t *e,
                       cxs_emit_target_t tgt, const char *fn) {
    if (e->jtab_len == 0) return;

    fprintf(fp, "\n/* Jump table (%d entries) — T9 indirect control flow */\n",
            e->jtab_len);
    fprintf(fp, "    .section .rodata\n");
    fprintf(fp, "    .align 8\n");
    fprintf(fp, "cxs_jtab_%s:\n", fn);

    for (int i = 0; i < e->jtab_len; i++) {
        if (tgt == CXS_EMIT_X86_64)
            fprintf(fp, "    .quad %s            /* slot[%d]  key=0x%"PRIx64" */\n",
                    e->jtab[i].label, i, (uint64_t)e->jtab[i].encode_key);
        else
            fprintf(fp, "    .xword %s           /* slot[%d]  key=0x%"PRIx64" */\n",
                    e->jtab[i].label, i, (uint64_t)e->jtab[i].encode_key);
    }
    fprintf(fp, "    .section .text\n\n");
}

/* ============================================================
 * Main emitter entry point
 * ============================================================ */

int cxs_emit_asm(cxs_engine_t *e, const cxs_emit_opts_t *opts) {
    cxs_emit_target_t tgt   = resolve_target(opts->target);
    const char       *fname = opts->outfile
                              ? opts->outfile
                              : cxs_emit_default_filename(tgt);
    const char       *fn    = opts->fn_name ? opts->fn_name : "cxs_fn";

    FILE *fp = fopen(fname, "w");
    if (!fp) {
        fprintf(stderr, "  [ERROR] Cannot open output file: %s\n", fname);
        return CXS_ERR;
    }

    printf("  Emitting %s assembly to: %s\n",
           (tgt == CXS_EMIT_ARM64) ? "AArch64" : "x86-64", fname);

    /* ── Prologue ── */
    if (tgt == CXS_EMIT_ARM64)
        write_prologue_arm64(fp, fn, e, opts->show_stats);
    else
        write_prologue_x64(fp, fn, e, opts->show_stats);

    /* ── Instruction stream ── */
    int emitted = 0;
    for (int i = 0; i < e->num_insns; i++) {
        insn_t *ins = &e->insns[i];

        /* ── Label anchor ── */
        if (ins->has_label) {
            /* CFI_START with .cfi_def_cfa_offset label: emit as directive on ARM64 */
            if (ins->type == INSN_CFI_START && tgt == CXS_EMIT_ARM64
                && strncmp(ins->label, ".cfi_def_cfa_offset", 19) == 0) {
                fprintf(fp, "\n    %s\n", ins->label);
                continue;  /* skip insn handler — directive already emitted */
            }
            /* CFI_START/.cfi_startproc and CFI_END: no label line, fall to insn handler */
            if (ins->type == INSN_CFI_START || ins->type == INSN_CFI_END) {
                /* handled by insn case below */
            } else {
                /* Blank line before non-CFF labels for readability */
                if (strncmp(ins->label, ".cff_", 5) != 0 &&
                    strncmp(ins->label, ".t9", 3) != 0)
                    fprintf(fp, "\n");
                fprintf(fp, "%s:\n", ins->label);
            }
        }

        /* ── Suppress pure decoration that has no ASM equivalent ── */
        if (ins->type == INSN_LABEL)     continue;   /* already printed above */
        if (ins->type == INSN_OVERLAP)   continue;   /* polyglot markers — no output */
        if (ins->type == INSN_DEAD_CALL) continue;   /* call to unreachable → suppress */
        /* Pure NOPs and JUNK from T2 do emit (fall through) */

        /* ── Emit ── */
        if (tgt == CXS_EMIT_ARM64)
            emit_arm64_insn(fp, ins, opts->annotate);
        else
            emit_x64_insn(fp, ins, opts->annotate);

        emitted++;
    }

    /* ── Jump table ── */
    write_jtab(fp, e, tgt, fn);

    /* ── Epilogue ── */
    if (tgt == CXS_EMIT_ARM64)
        write_epilogue_arm64(fp, fn);
    else
        write_epilogue_x64(fp, fn);

    fclose(fp);
    printf("  Done.  %d instructions emitted to %s\n", emitted, fname);
    return CXS_OK;
}
