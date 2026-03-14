/*
 * engine.c  —  CXS Engine Core  v4.0
 * ============================================================
 * Responsibilities:
 *   - Engine lifecycle (init / free)
 *   - xorshift64 PRNG
 *   - Architecture-aware register name tables
 *   - Disassembler  (prints every transform's annotations)
 *   - Statistics printer
 *   - Full VM interpreter  (ZF/SF/CF/OF/PF, indirect jumps, CFF state)
 *   - Semantic equivalence verifier
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cxs.h"

/* ============================================================
 * Architecture-aware register name tables
 * ============================================================ */
#if defined(CXS_ARCH_ARM64)
static const char *reg_names[REG_COUNT] = {
    "x0","x1","x2","x3","x4","x5","sp","fp",
    "x8","x9","x10","x11","x12","x13","x14","x15"
};
static const char *arch_tag __attribute__((unused)) = "AArch64";
static const char *arch_full = "ARM64 (AArch64)";

#elif defined(CXS_ARCH_X86_64)
static const char *reg_names[REG_COUNT] = {
    "rax","rbx","rcx","rdx","rsi","rdi","rsp","rbp",
    "r8","r9","r10","r11","r12","r13","r14","r15"
};
static const char *arch_tag __attribute__((unused)) = "x86-64";
static const char *arch_full = "x86-64 (AMD64/Intel64)";

#else
static const char *reg_names[REG_COUNT] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","r13","r14","r15"
};
static const char *arch_tag __attribute__((unused)) = "generic";
static const char *arch_full = "Generic (architecture-neutral)";
#endif

/* ============================================================
 * Mnemonic table  (order must match insn_type_t enum)
 * ============================================================ */
static const char *insn_names[INSN_COUNT] = {
    "nop","mov","add","sub","mul","imul","div","idiv",
    "xor","and","or","not","neg","shl","shr","push","pop",
    "jmp","je","jne","jl","jg","jle","jge","cmp","test",
    "call","ret","lea","xchg","inc","dec",
    "OVERLAP","JUNK","LABEL",
    "ST_MOV","ST_CMP","ijmp",
    "DEAD_CALL","FAKE_PUSH","FAKE_POP","XOR_STUB",
    "CPUID","RDTSC",
    /* T17 */ "BOGUS_ARG",
    /* T19 */ "VIRT_ENTER","VIRT_OP","VIRT_EXIT",
    /* T21 */ "LOOP_INIT","LOOP_CMP",
    /* T22 */ "CFI_START","CFI_END",
    /* T25 */ "OUTLINE_CALL","OUTLINE_RET",
    /* T26 */ "BSWAP","ror",
    /* T27 */ "CHKSUM",
    /* T29 */ "RDRAND",
    /* T30 */ "KEYSCHED"
};

/* ============================================================
 * Lifecycle
 * ============================================================ */
void cxs_engine_init(cxs_engine_t *e) {
    memset(e, 0, sizeof(*e));
    e->seed = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)e;
    for (int i = 0; i < REG_COUNT; i++) e->rmap.map[i] = (reg_id_t)i;
    e->rmap.locked[REG_RAX] = 1;
    e->rmap.locked[REG_RSP] = 1;
    e->rmap.locked[REG_RBP] = 1;
}

void cxs_engine_free(cxs_engine_t *e) { memset(e, 0, sizeof(*e)); }

/* ============================================================
 * PRNG  —  xorshift64
 * ============================================================ */
int cxs_rand_range(cxs_engine_t *e, int lo, int hi) {
    e->seed ^= e->seed << 13;
    e->seed ^= e->seed >> 7;
    e->seed ^= e->seed << 17;
    if (hi <= lo) return lo;
    return lo + (int)(e->seed % (unsigned)(hi - lo + 1));
}

const char *cxs_reg_name(reg_id_t r) {
    return ((unsigned)r < REG_COUNT) ? reg_names[r] : "??";
}

/* ============================================================
 * Disassembler helpers
 * ============================================================ */
static const char *insn_tag(const insn_t *ins) {
    if (ins->is_antiana)  return "[A15]";
    if (ins->is_stub)     return "[A14]";
    if (ins->is_stack)    return "[A13]";
    if (ins->is_split)    return "[A12]";
    if (ins->is_subst)    return "[A11]";
    if (ins->is_dead)     return "[A10]";
    if (ins->is_overlap)  return "[OV6]";
    if (ins->is_opaque)   return "[OP4]";
    if (ins->is_indirect) return "[I9] ";
    if (ins->is_flat)     return "[F7] ";
    if (ins->is_encoded)  return "[E8] ";
    if (ins->is_junk)     return "[J2] ";
    return "[R]  ";
}

static const char *insn_hint(const insn_t *ins) {
    if (ins->type == INSN_STATE_MOV)  return "; CFF state assignment";
    if (ins->type == INSN_STATE_CMP)  return "; CFF state dispatch";
    if (ins->type == INSN_IJMP)       return "; indirect via jump table";
    if (ins->type == INSN_DEAD_CALL)  return "; unreachable dead-block ref";
    if (ins->type == INSN_FAKE_PUSH)  return "; fake stack noise";
    if (ins->type == INSN_FAKE_POP)   return "; fake stack noise";
    if (ins->type == INSN_XOR_STUB)   return "; XOR decrypt stub";
    if (ins->type == INSN_CPUID)      return "; anti-analysis CPUID trap";
    if (ins->type == INSN_RDTSC)      return "; timing-check marker";
    if (ins->type == INSN_OVERLAP)    return "; polyglot offset confusion";
    if (ins->is_opaque)               return "; opaque predicate";
    if (ins->is_encoded)              return "; const encoded";
    if (ins->is_subst)                return "; opcode substituted";
    if (ins->is_split)                return "; data-flow split";
    if (ins->is_dead)                 return "; dead (unreachable)";
    return "";
}

/* Render operand list into a buffer (for width-controlled printf) */
static int render_ops(const insn_t *ins, char *buf, int sz) {
    int off = 0;
    for (int j = 0; j < ins->num_ops && off < sz-2; j++) {
        if (j) { buf[off++]=','; buf[off++]=' '; }
        const operand_t *op = &ins->ops[j];
        switch (op->type) {
            case OP_REG:
                off += snprintf(buf+off, sz-off, "%s", reg_names[op->reg]);
                break;
            case OP_IMM:
                if (op->imm < -0xFFFF || op->imm > 0xFFFF)
                    off += snprintf(buf+off, sz-off, "0x%"PRIx64, (uint64_t)op->imm);
                else
                    off += snprintf(buf+off, sz-off, "%"PRId64, op->imm);
                break;
            case OP_LABEL:
                off += snprintf(buf+off, sz-off, "%s", op->label);
                break;
            case OP_MEM:
                off += snprintf(buf+off, sz-off, "[%s+%"PRId64"]",
                                reg_names[op->mem.base], op->mem.disp);
                break;
            case OP_JTAB:
                off += snprintf(buf+off, sz-off, "slot[%d]", op->jtab_slot);
                break;
            default: break;
        }
    }
    buf[off] = '\0';
    return off;
}

/* ============================================================
 * Disassembler  —  main entry point
 * ============================================================ */
void cxs_disasm_print(cxs_engine_t *e) {
    printf("  Architecture : %s\n", arch_full);
    printf("  Instructions : %d total\n\n", e->num_insns);

    printf("  %-5s  %4s  %3s  %-10s  %-28s  %s\n",
           "TAG", "IDX", "BLK", "MNEMONIC", "OPERANDS", "ANNOTATION");
    printf("  %-5s  %4s  %3s  %-10s  %-28s  %s\n",
           "-----","----","---","----------",
           "----------------------------","----------");

    for (int i = 0; i < e->num_insns; i++) {
        insn_t *ins = &e->insns[i];

        if (ins->has_label)
            printf("\n  %48s%s:\n", "", ins->label);

        const char *tag = insn_tag(ins);
        const char *mne = (ins->type < INSN_COUNT) ? insn_names[ins->type] : "???";
        char blk_s[12] = "---";
        if (ins->block_id >= 0) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(blk_s, sizeof(blk_s), "%d", ins->block_id);
#pragma GCC diagnostic pop
        }
        printf("  %-5s  %4d  %-3s  %-10s  ", tag, i, blk_s, mne);

        /* Operands */
        if (ins->type == INSN_OVERLAP) {
            printf("bytes: ");
            for (int b = 0; b < ins->overlap_len; b++)
                printf("%02X ", ins->overlap_bytes[b]);
            printf("%-*s", 28 - 7 - ins->overlap_len*3, "");
        } else if (ins->type == INSN_IJMP) {
            char tmp[64] = "";
            if (ins->num_ops >= 1 && ins->ops[0].type == OP_REG)
                snprintf(tmp, sizeof(tmp), "*%s", reg_names[ins->ops[0].reg]);
            if (ins->num_ops >= 2 && ins->ops[1].type == OP_JTAB) {
                char app[24]; snprintf(app,sizeof(app)," (slot %d)",ins->ops[1].jtab_slot);
                strncat(tmp, app, sizeof(tmp)-strlen(tmp)-1);
            }
            printf("%-28s", tmp);
        } else {
            char op_buf[80] = "";
            render_ops(ins, op_buf, sizeof(op_buf));
            printf("%-28s", op_buf);
        }

        const char *hint = insn_hint(ins);
        if (*hint) printf("  %s", hint);
        printf("\n");
    }

    if (e->jtab_len > 0) {
        printf("\n  Jump Table (%d entries):\n", e->jtab_len);
        for (int i = 0; i < e->jtab_len; i++)
            printf("    slot[%2d]  ->  %-22s  (key=0x%08"PRIx64")\n",
                   i, e->jtab[i].label, (uint64_t)e->jtab[i].encode_key);
    }
    if (e->split_len > 0) {
        printf("\n  Variable Splits (%d):\n", e->split_len);
        for (int i = 0; i < e->split_len; i++) {
            split_rec_t *s = &e->splits[i];
            if (!s->active) continue;
            printf("    %s  ->  %s XOR %s  (mask=0x%"PRIx64")\n",
                   reg_names[s->original], reg_names[s->hi],
                   reg_names[s->lo], (uint64_t)s->mask);
        }
    }
}

/* ============================================================
 * Statistics printer
 * ============================================================ */
void cxs_stats_print(cxs_engine_t *e) {
    printf("  +--------------------------------------------------+\n");
    printf("  |            Transform Results Summary             |\n");
    printf("  +-------+----------------------------------+-------+\n");
    printf("  | Pass  |  Technique                       | Count |\n");
    printf("  +-------+----------------------------------+-------+\n");
    printf("  |  T1   |  Arithmetic substitution         | %5d |\n", e->stats.arith_subs);
    printf("  |  T2   |  Junk injection                  | %5d |\n", e->stats.junk_injected);
    printf("  |  T3   |  Block reordering                | %5d |\n", e->stats.blocks_reordered);
    printf("  |  T4   |  Opaque predicates               | %5d |\n", e->stats.opaque_inserted);
    printf("  |  T5   |  Register renaming               | %5d |\n", e->stats.regs_renamed);
    printf("  |  T6   |  Instruction overlap bytes       | %5d |\n", e->stats.overlap_inserted);
    printf("  |  T7   |  Control flow flattening (CFF)   | %5d |\n", e->stats.blocks_flattened);
    printf("  |  T8   |  Constant encoding               | %5d |\n", e->stats.consts_encoded);
    printf("  |  T9   |  Indirect control flow           | %5d |\n", e->stats.jumps_indirected);
    printf("  |  T10  |  Dead code insertion             | %5d |\n", e->stats.dead_blocks);
    printf("  |  T11  |  Instruction substitution        | %5d |\n", e->stats.insns_substituted);
    printf("  |  T12  |  Data flow obfuscation           | %5d |\n", e->stats.vars_split);
    printf("  |  T13  |  Stack frame mangling            | %5d |\n", e->stats.stack_noise);
    printf("  |  T14  |  Polymorphic decrypt stub        | %5d |\n", e->stats.stub_insns);
    printf("  |  T15  |  Anti-analysis markers           | %5d |\n", e->stats.antiana_markers);
    printf("  |  T16  |  Data encryption                 | %5d |\n", e->stats.data_encrypted);
    printf("  |  T17  |  Bogus function arguments        | %5d |\n", e->stats.bogus_args);
    printf("  |  T18  |  Instruction replication         | %5d |\n", e->stats.insns_replicated);
    printf("  |  T19  |  CFG virtualization lite         | %5d |\n", e->stats.virt_handlers);
    printf("  |  T20  |  Alias register chains           | %5d |\n", e->stats.alias_chains);
    printf("  |  T21  |  Loop unrolling noise            | %5d |\n", e->stats.loop_noise);
    printf("  |  T22  |  Exception frame noise           | %5d |\n", e->stats.cfi_markers);
    printf("  |  T23  |  Pointer obfuscation             | %5d |\n", e->stats.ptrs_encoded);
    printf("  |  T24  |  Constant propagation fake       | %5d |\n", e->stats.fakecp_chains);
    printf("  |  T25  |  Function outline noise          | %5d |\n", e->stats.outlined_stubs);
    printf("  |  T26  |  Bitfield extraction noise       | %5d |\n", e->stats.bitfield_seqs);
    printf("  |  T27  |  Checksum guards                 | %5d |\n", e->stats.chksum_guards);
    printf("  |  T28  |  Predicated move obfuscation     | %5d |\n", e->stats.cmov_chains);
    printf("  |  T29  |  Entropy injection               | %5d |\n", e->stats.entropy_insns);
    printf("  |  T30  |  Multi-layer key schedule        | %5d |\n", e->stats.keysched_rounds);
    printf("  +-------+----------------------------------+-------+\n");
    printf("  |        Total transform passes            |    30 |\n");
    printf("  |        Final instruction count           | %5d |\n", e->num_insns);
    printf("  +--------------------------------------------------+\n");
}

/* ============================================================
 * VM  —  full flag-accurate interpreter
 * ============================================================ */
typedef struct { int64_t regs[REG_COUNT]; cpu_flags_t fl; } vm_state_t;

static int64_t vm_get(const vm_state_t *vm, const operand_t *op) {
    if (op->type == OP_REG) return vm->regs[op->reg];
    if (op->type == OP_IMM) return op->imm;
    return 0;
}
static void vm_set(vm_state_t *vm, const operand_t *op, int64_t v) {
    if (op->type == OP_REG) vm->regs[op->reg] = v;
}

static void vm_flags_add(cpu_flags_t *fl, int64_t a, int64_t b, int64_t r) {
    fl->zf=(r==0); fl->sf=(r<0);
    fl->cf=((uint64_t)r<(uint64_t)a);
    fl->of=(~(a^b)&(a^r))<0;
    uint8_t lo=(uint8_t)(r&0xFF);
    lo^=lo>>4; lo^=lo>>2; lo^=lo>>1; fl->pf=!(lo&1);
}
static void vm_flags_sub(cpu_flags_t *fl, int64_t a, int64_t b, int64_t r) {
    fl->zf=(r==0); fl->sf=(r<0);
    fl->cf=((uint64_t)a<(uint64_t)b);
    fl->of=((a^b)&(a^r))<0;
    uint8_t lo=(uint8_t)(r&0xFF);
    lo^=lo>>4; lo^=lo>>2; lo^=lo>>1; fl->pf=!(lo&1);
}
static void vm_flags_logic(cpu_flags_t *fl, int64_t r) {
    fl->zf=(r==0); fl->sf=(r<0); fl->cf=0; fl->of=0;
    uint8_t lo=(uint8_t)(r&0xFF);
    lo^=lo>>4; lo^=lo>>2; lo^=lo>>1; fl->pf=!(lo&1);
}

static int vm_find_label(const cxs_engine_t *e, const char *lbl) {
    for (int i = 0; i < e->num_insns; i++)
        if (e->insns[i].has_label && strcmp(e->insns[i].label, lbl)==0)
            return i;
    return -1;
}

static int vm_jcc(const cpu_flags_t *fl, insn_type_t t) {
    switch(t) {
        case INSN_JE:  return  fl->zf;
        case INSN_JNE: return !fl->zf;
        case INSN_JL:  return  (fl->sf!=fl->of);
        case INSN_JG:  return (!fl->zf && fl->sf==fl->of);
        case INSN_JLE: return  (fl->zf || fl->sf!=fl->of);
        case INSN_JGE: return  (fl->sf==fl->of);
        default:       return 0;
    }
}

static int64_t vm_run(cxs_engine_t *e, vm_state_t *vm) {
    int ip=0, steps=0;
    while (ip < e->num_insns && steps < 500000) {
        insn_t *ins = &e->insns[ip];
        steps++;

        /* Skip all decoration / marker instructions */
        if (ins->is_junk || ins->is_opaque || ins->is_overlap ||
            ins->is_dead || ins->is_stack  || ins->is_stub    ||
            ins->is_antiana ||
            ins->type==INSN_NOP      || ins->type==INSN_LABEL    ||
            ins->type==INSN_JUNK     || ins->type==INSN_OVERLAP  ||
            ins->type==INSN_DEAD_CALL|| ins->type==INSN_FAKE_PUSH||
            ins->type==INSN_FAKE_POP || ins->type==INSN_XOR_STUB ||
            ins->type==INSN_CPUID   || ins->type==INSN_RDTSC) {
            ip++; continue;
        }

        switch (ins->type) {
            case INSN_MOV:  vm_set(vm,&ins->ops[0],vm_get(vm,&ins->ops[1])); ip++; break;
            case INSN_ADD:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]),r=a+b;
                              vm_set(vm,&ins->ops[0],r); vm_flags_add(&vm->fl,a,b,r); ip++; break; }
            case INSN_SUB:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]),r=a-b;
                              vm_set(vm,&ins->ops[0],r); vm_flags_sub(&vm->fl,a,b,r); ip++; break; }
            case INSN_IMUL: { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]);
                              vm_set(vm,&ins->ops[0],a*b); ip++; break; }
            case INSN_MUL:  { uint64_t a=(uint64_t)vm_get(vm,&ins->ops[0]),b=(uint64_t)vm_get(vm,&ins->ops[1]);
                              vm_set(vm,&ins->ops[0],(int64_t)(a*b)); ip++; break; }
            case INSN_INC:  { int64_t a=vm_get(vm,&ins->ops[0]),r=a+1;
                              vm_set(vm,&ins->ops[0],r);
                              vm->fl.zf=(r==0);vm->fl.sf=(r<0);vm->fl.of=(a==INT64_MAX); ip++; break; }
            case INSN_DEC:  { int64_t a=vm_get(vm,&ins->ops[0]),r=a-1;
                              vm_set(vm,&ins->ops[0],r);
                              vm->fl.zf=(r==0);vm->fl.sf=(r<0);vm->fl.of=(a==INT64_MIN); ip++; break; }
            case INSN_NEG:  { int64_t a=vm_get(vm,&ins->ops[0]),r=-a;
                              vm_set(vm,&ins->ops[0],r); vm_flags_sub(&vm->fl,0,a,r); ip++; break; }
            case INSN_XOR:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]),r=a^b;
                              vm_set(vm,&ins->ops[0],r); vm_flags_logic(&vm->fl,r); ip++; break; }
            case INSN_AND:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]),r=a&b;
                              vm_set(vm,&ins->ops[0],r); vm_flags_logic(&vm->fl,r); ip++; break; }
            case INSN_OR:   { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]),r=a|b;
                              vm_set(vm,&ins->ops[0],r); vm_flags_logic(&vm->fl,r); ip++; break; }
            case INSN_NOT:  { vm_set(vm,&ins->ops[0],~vm_get(vm,&ins->ops[0])); ip++; break; }
            case INSN_SHL:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1])&63;
                              int64_t r=(b<64)?(int64_t)((uint64_t)a<<b):0;
                              vm_set(vm,&ins->ops[0],r); vm->fl.zf=(r==0);vm->fl.sf=(r<0); ip++; break; }
            case INSN_SHR:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1])&63;
                              int64_t r=(b<64)?(int64_t)((uint64_t)a>>b):0;
                              vm_set(vm,&ins->ops[0],r); vm->fl.zf=(r==0);vm->fl.sf=(r<0); ip++; break; }
            case INSN_XCHG: { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]);
                              vm_set(vm,&ins->ops[0],b); vm_set(vm,&ins->ops[1],a); ip++; break; }
            case INSN_CMP:  { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]);
                              vm_flags_sub(&vm->fl,a,b,a-b); ip++; break; }
            case INSN_TEST: { int64_t a=vm_get(vm,&ins->ops[0]),b=vm_get(vm,&ins->ops[1]);
                              vm_flags_logic(&vm->fl,a&b); ip++; break; }
            case INSN_JMP:  {
                if (ins->ops[0].type==OP_LABEL) {
                    int t=vm_find_label(e,ins->ops[0].label);
                    if(t>=0){ip=t;break;}
                }
                ip++; break;
            }
            case INSN_JE: case INSN_JNE: case INSN_JL:
            case INSN_JG: case INSN_JLE: case INSN_JGE: {
                if (vm_jcc(&vm->fl,ins->type) && ins->ops[0].type==OP_LABEL) {
                    int t=vm_find_label(e,ins->ops[0].label);
                    if(t>=0){ip=t;break;}
                }
                ip++; break;
            }
            case INSN_RET:  goto done;
            /* T7: CFF state machine */
            case INSN_STATE_MOV: {
                vm->regs[REG_STATE]=(ins->num_ops>=2)?vm_get(vm,&ins->ops[1]):0;
                ip++; break;
            }
            case INSN_STATE_CMP: {
                int64_t a=vm->regs[REG_STATE];
                int64_t b=(ins->num_ops>=2)?vm_get(vm,&ins->ops[1]):0;
                vm_flags_sub(&vm->fl,a,b,a-b); ip++; break;
            }
            /* T9: indirect jump through jump table */
            case INSN_IJMP: {
                if (ins->num_ops>=2 && ins->ops[1].type==OP_JTAB) {
                    int slot=ins->ops[1].jtab_slot;
                    if (slot>=0 && slot<e->jtab_len) {
                        int t=vm_find_label(e,e->jtab[slot].label);
                        if(t>=0){ip=t;break;}
                    }
                }
                ip++; break;
            }
            default: ip++; break;
        }
    }
done:
    return vm->regs[REG_R0];
}

int64_t cxs_execute(cxs_engine_t *e, int64_t input) {
    vm_state_t vm; memset(&vm,0,sizeof(vm));
    vm.regs[REG_R0]=input;
    return vm_run(e,&vm);
}

/* ============================================================
 * Semantic equivalence verifier
 *
 * Golden values are captured from the ORIGINAL (pre-transform) IR
 * via cxs_capture_golden(), called before cxs_run_pipeline().
 * This makes verification work for ANY .cxs program, not just
 * the built-in sample f(x)=(x+5-3)*2.
 * ============================================================ */

static const int64_t _verify_inputs[] = {
    0,1,-1,2,10,-10,100,-100,42,127,-127,255,-255,
    1000,-1000,32767,-32767,7,-7,13,99,-99,3,-3,500,-500,
    65535,-65535,1000000,-1000000,INT32_MAX,INT32_MIN
};
#define VERIFY_N ((int)(sizeof(_verify_inputs)/sizeof(_verify_inputs[0])))

/* Stored golden outputs — populated by cxs_capture_golden() */
static int64_t _golden[32];
static int     _golden_valid = 0;

void cxs_capture_golden(cxs_engine_t *e) {
    for (int i = 0; i < VERIFY_N; i++)
        _golden[i] = cxs_execute(e, _verify_inputs[i]);
    _golden_valid = 1;
}

int64_t cxs_golden_for(int64_t input) {
    for (int i = 0; i < VERIFY_N; i++)
        if (_verify_inputs[i] == input) return _golden[i];
    return (input + 2) * 2; /* fallback — should not happen */
}

int cxs_verify_equivalence(cxs_engine_t *e) {
    int passed = 0;
    for (int i = 0; i < VERIFY_N; i++) {
        int64_t x        = _verify_inputs[i];
        int64_t expected = _golden_valid ? _golden[i] : (x + 2) * 2;
        int64_t got      = cxs_execute(e, x);
        if (got == expected) {
            passed++;
        } else {
            printf("    [MISMATCH] input=%-10"PRId64
                   "  expected=%-10"PRId64"  got=%-10"PRId64"\n",
                   x, expected, got);
        }
    }
    return (passed == VERIFY_N) ? CXS_OK : CXS_ERR;
}
