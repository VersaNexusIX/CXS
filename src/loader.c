/*
 * loader.c  —  CXS Instruction Loaders  v4.0
 * ============================================================
 * Two load paths:
 *
 *  1. cxs_load_sample_block()
 *     Built-in golden program:  f(x) = (x + 5 - 3) * 2
 *     Used by the interactive demo and --stress mode.
 *
 *  2. cxs_load_file(e, path)
 *     Parses a simple CXS IR text file.
 *
 * CXS IR file format (.cxs):
 * ──────────────────────────
 *  Lines starting with '#' are comments.
 *  Empty lines are ignored.
 *
 *  Directives:
 *    .block <name>          begin a new basic block
 *    .end                   end current basic block
 *
 *  Instructions:
 *    <mnemonic> [dst [, src]]
 *
 *  Operand syntax:
 *    rax / x0 / r0          register (arch name or abstract r<N>)
 *    42 / -7 / 0xDEAD       immediate integer
 *    .label_name            label reference
 *    [rax+8]                memory operand
 *
 *  Example:
 *    # my function: f(x) = x * 3 + 1
 *    .block .entry
 *      label .entry
 *      imul  rax, 3
 *      add   rax, 1
 *      jmp   .done
 *    .end
 *    .block .done
 *      label .done
 *      ret
 *    .end
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include "cxs.h"

/* ============================================================
 * Emit helpers  (shared by both load paths)
 * ============================================================ */

static insn_t *emit(cxs_engine_t *e, insn_type_t type) {
    if (e->num_insns >= CXS_MAX_INSN) return NULL;
    insn_t *ins = &e->insns[e->num_insns++];
    memset(ins, 0, sizeof(*ins));
    ins->type     = type;
    ins->block_id = -1;
    return ins;
}

static operand_t op_reg(reg_id_t r) {
    operand_t o = {0}; o.type = OP_REG; o.reg = r; return o;
}
static operand_t op_imm(int64_t v) {
    operand_t o = {0}; o.type = OP_IMM; o.imm = v; return o;
}
static operand_t op_lbl(const char *n) {
    operand_t o = {0}; o.type = OP_LABEL;
    strncpy(o.label, n, CXS_LABEL_LEN-1); return o;
}

static void set_label(insn_t *ins, const char *n) {
    ins->has_label = 1;
    strncpy(ins->label, n, CXS_LABEL_LEN-1);
}

static void begin_block(cxs_engine_t *e, const char *lbl, int *idx) {
    if (e->num_blocks >= CXS_MAX_BLOCKS) return;
    block_t *blk  = &e->blocks[e->num_blocks];
    blk->id       = e->num_blocks;
    blk->start    = e->num_insns;
    blk->end      = blk->start;
    blk->next_id  = -1;
    blk->is_dead  = 0;
    strncpy(blk->label, lbl, CXS_LABEL_LEN-1);
    *idx = e->num_blocks++;
}

static void end_block(cxs_engine_t *e, int idx, int next_id) {
    e->blocks[idx].end     = e->num_insns;
    e->blocks[idx].next_id = next_id;
}

static void assign_block_ids(cxs_engine_t *e) {
    for (int b = 0; b < e->num_blocks; b++) {
        block_t *blk = &e->blocks[b];
        for (int i = blk->start; i < blk->end; i++)
            e->insns[i].block_id = b;
    }
}

/* ============================================================
 * Built-in sample program
 *
 * f(x) = (x + 5 - 3) * 2   =>   golden:  (x + 2) * 2
 *
 * Block structure:
 *   .entry    : add rax,5 / sub rax,3 / jmp .multiply
 *   .multiply : imul rax,2 / jmp .done
 *   .done     : ret
 * ============================================================ */
void cxs_load_sample_block(cxs_engine_t *e) {
    int b0, b1, b2;
    insn_t *ins;

    /* ── block 0: entry ── */
    begin_block(e, ".entry", &b0);

    ins = emit(e, INSN_LABEL);   set_label(ins, ".entry");

    ins = emit(e, INSN_ADD);
    ins->ops[0]=op_reg(REG_RAX); ins->ops[1]=op_imm(5); ins->num_ops=2;

    ins = emit(e, INSN_SUB);
    ins->ops[0]=op_reg(REG_RAX); ins->ops[1]=op_imm(3); ins->num_ops=2;

    ins = emit(e, INSN_JMP);
    ins->ops[0]=op_lbl(".multiply"); ins->num_ops=1;

    end_block(e, b0, 1);

    /* ── block 1: multiply ── */
    begin_block(e, ".multiply", &b1);

    ins = emit(e, INSN_LABEL);   set_label(ins, ".multiply");

    ins = emit(e, INSN_IMUL);
    ins->ops[0]=op_reg(REG_RAX); ins->ops[1]=op_imm(2); ins->num_ops=2;

    ins = emit(e, INSN_JMP);
    ins->ops[0]=op_lbl(".done"); ins->num_ops=1;

    end_block(e, b1, 2);

    /* ── block 2: done ── */
    begin_block(e, ".done", &b2);

    ins = emit(e, INSN_LABEL);   set_label(ins, ".done");
    ins = emit(e, INSN_RET);     ins->num_ops=0;

    end_block(e, b2, -1);

    assign_block_ids(e);

    e->num_exec_order=3;
    e->exec_order[0]=0; e->exec_order[1]=1; e->exec_order[2]=2;

    printf("    Loaded built-in sample: f(x) = (x+5-3)*2\n");
    printf("    Blocks: %d   Instructions: %d\n", e->num_blocks, e->num_insns);
}

/* ============================================================
 * File loader  —  parse a .cxs IR text file
 * ============================================================ */

/* Map a register name string → reg_id_t.
 * Accepts both x86 names (rax, rbx…) and abstract names (r0…r15),
 * as well as AArch64 names (x0…x15, sp, fp). */
static reg_id_t parse_reg(const char *s) {
    struct { const char *name; reg_id_t id; } tbl[] = {
        /* x86-64 */
        {"rax",REG_R0},{"rbx",REG_R1},{"rcx",REG_R2},{"rdx",REG_R3},
        {"rsi",REG_R4},{"rdi",REG_R5},{"rsp",REG_R6},{"rbp",REG_R7},
        {"r8", REG_R8},{"r9", REG_R9},{"r10",REG_R10},{"r11",REG_R11},
        {"r12",REG_R12},{"r13",REG_R13},{"r14",REG_R14},{"r15",REG_R15},
        /* AArch64 */
        {"x0", REG_R0},{"x1", REG_R1},{"x2", REG_R2},{"x3", REG_R3},
        {"x4", REG_R4},{"x5", REG_R5},{"sp", REG_R6},{"fp", REG_R7},
        {"x8", REG_R8},{"x9", REG_R9},{"x10",REG_R10},{"x11",REG_R11},
        {"x12",REG_R12},{"x13",REG_R13},{"x14",REG_R14},{"x15",REG_R15},
        /* abstract */
        {"r0", REG_R0},{"r1", REG_R1},{"r2", REG_R2},{"r3", REG_R3},
        {"r4", REG_R4},{"r5", REG_R5},{"r6", REG_R6},{"r7", REG_R7},
        {NULL, REG_R0}
    };
    for (int i = 0; tbl[i].name; i++)
        if (strcasecmp(s, tbl[i].name) == 0) return tbl[i].id;
    return (reg_id_t)-1;
}

/* Parse a single operand token → operand_t */
static operand_t parse_operand(const char *tok) {
    operand_t op = {0};

    if (!tok || !*tok) { op.type = OP_NONE; return op; }

    /* Label reference: starts with '.' */
    if (tok[0] == '.') {
        op.type = OP_LABEL;
        strncpy(op.label, tok, CXS_LABEL_LEN-1);
        return op;
    }

    /* Memory: [reg+disp] */
    if (tok[0] == '[') {
        op.type = OP_MEM;
        char tmp[64]; strncpy(tmp, tok+1, 62); tmp[63]='\0';
        char *rb = strchr(tmp, ']'); if(rb) *rb='\0';
        char *plus = strchr(tmp, '+');
        if (plus) {
            *plus = '\0';
            op.mem.disp = strtoll(plus+1, NULL, 0);
        }
        op.mem.base = parse_reg(tmp);
        return op;
    }

    /* Register */
    reg_id_t rid = parse_reg(tok);
    if ((int)rid >= 0) {
        op.type = OP_REG; op.reg = rid; return op;
    }

    /* Immediate (decimal or 0x hex) */
    op.type = OP_IMM;
    op.imm  = strtoll(tok, NULL, 0);
    return op;
}

/* Map mnemonic string → insn_type_t */
static insn_type_t parse_mnemonic(const char *m) {
    struct { const char *name; insn_type_t type; } tbl[] = {
        {"nop",INSN_NOP},{"mov",INSN_MOV},{"add",INSN_ADD},
        {"sub",INSN_SUB},{"mul",INSN_MUL},{"imul",INSN_IMUL},
        {"div",INSN_DIV},{"idiv",INSN_IDIV},
        {"xor",INSN_XOR},{"and",INSN_AND},{"or",INSN_OR},
        {"not",INSN_NOT},{"neg",INSN_NEG},
        {"shl",INSN_SHL},{"shr",INSN_SHR},
        {"push",INSN_PUSH},{"pop",INSN_POP},
        {"jmp",INSN_JMP},{"je",INSN_JE},{"jne",INSN_JNE},
        {"jl",INSN_JL},{"jg",INSN_JG},{"jle",INSN_JLE},{"jge",INSN_JGE},
        {"cmp",INSN_CMP},{"test",INSN_TEST},
        {"call",INSN_CALL},{"ret",INSN_RET},
        {"lea",INSN_LEA},{"xchg",INSN_XCHG},
        {"inc",INSN_INC},{"dec",INSN_DEC},
        {"label",INSN_LABEL},
        {NULL, INSN_NOP}
    };
    for (int i = 0; tbl[i].name; i++)
        if (strcasecmp(m, tbl[i].name) == 0) return tbl[i].type;
    return (insn_type_t)-1;
}

/* Strip leading/trailing whitespace (in-place) */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e-1))) *--e = '\0';
    return s;
}

int cxs_load_file(cxs_engine_t *e, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "  [ERROR] Cannot open file: %s\n", path);
        return CXS_NOFILE;
    }

    strncpy(e->source_file, path, sizeof(e->source_file)-1);
    e->has_source = 1;

    char   line[512];
    int    lineno      = 0;
    int    cur_block   = -1;
    int    blocks_done = 0;

    printf("    Parsing file: %s\n", path);

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *s = trim(line);

        /* Skip blanks and comments */
        if (!*s || s[0] == '#') continue;

        /* ── .block <name> — begin a basic block ── */
        if (strncmp(s, ".block", 6) == 0) {
            char *name = trim(s + 6);
            if (!*name) snprintf(name = line, 48, ".block_%d", blocks_done);

            if (cur_block >= 0) {
                /* Auto-close previous block */
                end_block(e, cur_block, e->num_blocks); /* tentative next */
            }

            begin_block(e, name, &cur_block);
                blocks_done++;
            continue;
        }

        /* ── .end — close current block ── */
        if (strcmp(s, ".end") == 0) {
            if (cur_block >= 0)
                end_block(e, cur_block,
                          (e->num_blocks < CXS_MAX_BLOCKS) ? e->num_blocks : -1);
            cur_block = -1;
            continue;
        }

        /* ── Instruction line ── */
        /* Tokenise: mnemonic [op1 [, op2 [, op3]]] */
        char copy[512]; strncpy(copy, s, 511);
        char *mne_tok = strtok(copy, " \t");
        if (!mne_tok) continue;

        insn_type_t itype = parse_mnemonic(mne_tok);
        if ((int)itype < 0) {
            fprintf(stderr,
                    "  [WARN] line %d: unknown mnemonic '%s' — skipped\n",
                    lineno, mne_tok);
            continue;
        }

        insn_t *ins = emit(e, itype);
        if (!ins) {
            fprintf(stderr, "  [ERROR] Instruction buffer full at line %d\n", lineno);
            break;
        }

        /* Special handling: label <name> sets anchor */
        if (itype == INSN_LABEL) {
            char *lbl_name = trim(strtok(NULL, ""));
            if (lbl_name && *lbl_name) set_label(ins, lbl_name);
            continue;
        }

        /* Parse up to 3 comma-separated operands */
        char *rest = strtok(NULL, "");  /* remainder of line */
        if (rest) {
            rest = trim(rest);
            char *tok;
            /* Replace commas with \0 and collect tokens */
            char rbuf[256]; strncpy(rbuf, rest, 255);
            int nops = 0;
            tok = strtok(rbuf, ",");
            while (tok && nops < CXS_OPERAND_MAX) {
                ins->ops[nops++] = parse_operand(trim(tok));
                tok = strtok(NULL, ",");
            }
            ins->num_ops = nops;
        }
    }

    /* Close any unclosed final block */
    if (cur_block >= 0)
        end_block(e, cur_block, -1);

    fclose(fp);

    /* If no blocks were defined, wrap everything in one synthetic block */
    if (e->num_blocks == 0 && e->num_insns > 0) {
        e->blocks[0].id    = 0;
        e->blocks[0].start = 0;
        e->blocks[0].end   = e->num_insns;
        e->blocks[0].next_id = -1;
        strncpy(e->blocks[0].label, ".auto_block", CXS_LABEL_LEN-1);
        e->num_blocks = 1;
    }

    /* Fix up tentative next_id values & exec order */
    e->num_exec_order = e->num_blocks;
    for (int b = 0; b < e->num_blocks; b++) {
        e->exec_order[b] = b;
        if (e->blocks[b].next_id >= e->num_blocks)
            e->blocks[b].next_id = -1;
    }

    assign_block_ids(e);

    printf("    Blocks: %d   Instructions: %d   Source lines: %d\n",
           e->num_blocks, e->num_insns, lineno);

    return (e->num_insns > 0) ? CXS_OK : CXS_ERR;
}
