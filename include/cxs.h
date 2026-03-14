/*
 * cxs.h  —  CXS Engine Public API  v4.0
 * ============================================================
 * Code eXecution Scrambler | Polymorphic Self-Modifying ASM Engine
 *
 * Transform pipeline (30 techniques):
 *
 *  LAYER 1 — Instruction-level
 *   T1   Arithmetic substitution     ADD↔SUB, INC→ADD 1, IMUL 2→ADD self
 *   T2   Junk injection              dead NOP/XOR/AND sleds
 *   T6   Instruction overlap bytes   polyglot byte sequences (real ASM)
 *   T11  Instruction substitution    semantic-equivalent opcode swaps
 *   T18  Instruction replication     semantically-neutral instruction clones
 *   T26  Bitfield extraction noise   fake BSWAP/ROR/shift sequences
 *
 *  LAYER 2 — Data / operand
 *   T5   Register renaming           Fisher-Yates permutation of scratch pool
 *   T8   Constant encoding           XOR/ADD/ROL obfuscation of immediates
 *   T12  Data flow obfuscation       variable splitting across register pairs
 *   T13  Stack frame mangling        fake push/pop + ESP arithmetic noise
 *   T16  Data encryption             rolling-XOR encryption of inline constants
 *   T20  Alias register chains       r_alias=orig → use alias → restore chain
 *   T23  Pointer obfuscation         base+encoded_offset address encoding
 *   T24  Constant propagation fake   fake CProp candidate chains
 *   T28  Predicated move obfuscation MOV→CMOV ladder (conditional move chains)
 *
 *  LAYER 3 — Control flow
 *   T3   Block reordering            shuffle basic blocks + JMP glue
 *   T4   Opaque predicates           always-false branches (5 math patterns)
 *   T7   Control flow flattening     state-machine dispatch loop
 *   T9   Indirect control flow       jump table + XOR-encoded targets
 *   T10  Dead code insertion         unreachable fake function call blocks
 *   T17  Bogus function arguments    fake arg-setup MOV sequences
 *   T19  CFG virtualization lite     mini-VM bytecode handler dispatch
 *   T21  Loop unrolling noise        fake counted loop (always 1 iteration)
 *   T25  Function outline noise      fake helper CALL+RET outline stubs
 *
 *  LAYER 4 — Anti-analysis
 *   T14  Polymorphic decryption stub XOR self-modifying decode prologue
 *   T15  Anti-analysis markers       CPUID/RDTSC traps, fake exception refs
 *   T22  Exception frame noise       fake .cfi_startproc / SEH frame markers
 *   T27  Checksum guards             rolling XOR integrity check stubs
 *   T29  Entropy injection           RDRAND/PRNG seed sequences
 *   T30  Multi-layer key schedule    multi-round XOR-ADD-ROL key expansion
 *
 * Architecture support:
 *   CXS_ARCH_X86_64  — Linux / macOS / BSD / Windows  x86-64
 *   CXS_ARCH_ARM64   — Android / iOS / Apple Silicon  AArch64
 *   CXS_ARCH_GENERIC — pure-C fallback, arch-agnostic names
 *
 * Build:
 *   make                     auto-detect platform + arch
 *   make DEBUG=1             debug symbols
 *   make stress              50-cycle equivalence test
 *
 * CLI usage:
 *   cxs                      interactive demo (built-in sample program)
 *   cxs -f <file>            obfuscate a target IR file
 *   cxs -f <file> --stress N run N verify cycles on the file
 *   cxs --stress N           stress test built-in sample
 *   cxs --emit-asm           emit obfuscated assembly to .S file
 *   cxs --help               full usage reference
 * ============================================================
 */

#ifndef CXS_H
#define CXS_H

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

/* ============================================================
 * Architecture detection
 * Only auto-detect when Makefile has not set CXS_ARCH_* explicitly.
 * ============================================================ */
#if !defined(CXS_ARCH_ARM64) && !defined(CXS_ARCH_X86_64) && !defined(CXS_ARCH_GENERIC)
#  if   defined(__aarch64__) || defined(__arm64__)
#    define CXS_ARCH_ARM64
#  elif defined(__x86_64__)  || defined(_M_X64)
#    define CXS_ARCH_X86_64
#  else
#    define CXS_ARCH_GENERIC
#  endif
#endif

/* ============================================================
 * Return codes
 * ============================================================ */
#define CXS_OK       0
#define CXS_ERR     -1
#define CXS_OOM     -2
#define CXS_BOUNDS  -3
#define CXS_NOFILE  -4

/* ============================================================
 * Capacity limits
 * ============================================================ */
#define CXS_MAX_INSN      4096   /* T16-T30 add significant instruction count  */
#define CXS_MAX_BLOCKS     128   /* more blocks from T19/T21/T25               */
#define CXS_MAX_JUNK        16
#define CXS_OPERAND_MAX      4
#define CXS_LABEL_LEN       64
#define CXS_MAX_JTAB       128   /* T9 + T19 dispatch tables                   */
#define CXS_MAX_SPLIT       16   /* T12/T20 variable split + alias pairs        */
#define CXS_MAX_DEAD_BLOCKS 12   /* T10 + T25 fake blocks                      */
#define CXS_MAX_ALIAS       16   /* T20 alias register chain slots             */
#define CXS_MAX_OUTLINE     12   /* T25 outlined stub slots                    */
#define CXS_MAX_HANDLERS    32   /* T19 bytecode handler slots                 */
#define CXS_KEYSCH_ROUNDS    8   /* T30 key schedule rounds                    */

/* ============================================================
 * Instruction types  (architecture-neutral IR)
 * ============================================================ */
typedef enum {
    /* ── Arithmetic ── */
    INSN_NOP = 0,
    INSN_MOV,
    INSN_ADD,
    INSN_SUB,
    INSN_MUL,
    INSN_IMUL,
    INSN_DIV,
    INSN_IDIV,
    INSN_XOR,
    INSN_AND,
    INSN_OR,
    INSN_NOT,
    INSN_NEG,
    INSN_SHL,
    INSN_SHR,
    INSN_PUSH,
    INSN_POP,
    /* ── Control flow ── */
    INSN_JMP,
    INSN_JE,
    INSN_JNE,
    INSN_JL,
    INSN_JG,
    INSN_JLE,
    INSN_JGE,
    INSN_CMP,
    INSN_TEST,
    INSN_CALL,
    INSN_RET,
    INSN_LEA,
    INSN_XCHG,
    INSN_INC,
    INSN_DEC,
    /* ── Pseudo / decoration ── */
    INSN_OVERLAP,    /* T6 : overlap byte marker (skipped by VM)         */
    INSN_JUNK,       /* T2 : dead injected instruction                   */
    INSN_LABEL,      /* pseudo: label anchor                             */
    /* ── T7 state machine ── */
    INSN_STATE_MOV,  /* MOV __state, imm  (drives CFF dispatcher)        */
    INSN_STATE_CMP,  /* CMP __state, imm                                 */
    /* ── T9 indirect jump ── */
    INSN_IJMP,       /* JMP *reg  (through jump table)                   */
    /* ── T10 dead call ── */
    INSN_DEAD_CALL,  /* CALL <fake_label>  (unreachable block reference)  */
    /* ── T13 stack noise ── */
    INSN_FAKE_PUSH,  /* cosmetic PUSH (balanced by FAKE_POP, VM skips)   */
    INSN_FAKE_POP,   /* cosmetic POP                                      */
    /* ── T14 XOR decrypt stub ── */
    INSN_XOR_STUB,   /* self-modifying XOR key load (displayed only)     */
    /* ── T15 anti-analysis ── */
    INSN_CPUID,      /* CPUID trap marker                                 */
    INSN_RDTSC,      /* timing check marker                              */
    /* ── T17 bogus args ── */
    INSN_BOGUS_ARG,  /* fake argument setup (MOV reg, imm — no effect)   */
    /* ── T19 CFG virtualization ── */
    INSN_VIRT_ENTER, /* enter mini-VM bytecode dispatch                  */
    INSN_VIRT_OP,    /* single virtualized opcode inside dispatch         */
    INSN_VIRT_EXIT,  /* exit mini-VM, restore real result                 */
    /* ── T21 loop noise ── */
    INSN_LOOP_INIT,  /* fake loop counter init (MOV rcx, 1)              */
    INSN_LOOP_CMP,   /* fake loop branch (CMP/JNE back — never taken)    */
    /* ── T22 frame markers ── */
    INSN_CFI_START,  /* .cfi_startproc / fake SEH push marker            */
    INSN_CFI_END,    /* .cfi_endproc marker                              */
    /* ── T25 outline stubs ── */
    INSN_OUTLINE_CALL, /* CALL to outlined helper stub                   */
    INSN_OUTLINE_RET,  /* RET inside outlined stub                       */
    /* ── T26 bitfield noise ── */
    INSN_BSWAP,      /* BSWAP scratch reg                                */
    INSN_ROR,        /* ROR scratch reg, imm                             */
    /* ── T27 checksum guard ── */
    INSN_CHKSUM,     /* rolling XOR checksum accumulate                  */
    /* ── T29 entropy ── */
    INSN_RDRAND,     /* RDRAND / fake PRNG seed load                     */
    /* ── T30 key schedule ── */
    INSN_KEYSCHED,   /* multi-round key expansion step                   */
    INSN_COUNT
} insn_type_t;

/* ============================================================
 * Operand types
 * ============================================================ */
typedef enum {
    OP_NONE = 0,
    OP_REG,
    OP_IMM,
    OP_MEM,
    OP_LABEL,
    OP_JTAB    /* T9: jump-table slot index */
} operand_type_t;

/* ============================================================
 * Abstract Register IDs
 *
 *  Slot  │ x86-64  │ AArch64 │ Role
 *  ──────┼─────────┼─────────┼─────────────────────────────
 *  R0    │ rax     │ x0      │ return value / accumulator
 *  R1    │ rbx     │ x1      │ callee-saved / 2nd arg
 *  R2    │ rcx     │ x2      │ 3rd arg / scratch
 *  R3    │ rdx     │ x3      │ 4th arg / scratch
 *  R4    │ rsi     │ x4      │ 5th arg
 *  R5    │ rdi     │ x5      │ 6th arg / primary input
 *  R6    │ rsp     │ sp      │ stack pointer  [LOCKED]
 *  R7    │ rbp     │ fp      │ frame pointer  [LOCKED]
 *  R8    │ r8      │ x8      │ extended arg
 *  R9    │ r9      │ x9      │ extended arg
 *  R10   │ r10     │ x10     │ scratch  (T2/T4/T5 pool)
 *  R11   │ r11     │ x11     │ scratch  pool
 *  R12   │ r12     │ x12     │ scratch  pool  / T12 split-hi
 *  R13   │ r13     │ x13     │ T7 __state register
 *  R14   │ r14     │ x14     │ T9 indirect jump target
 *  R15   │ r15     │ x15     │ T8 constant decode temp / T12 split-lo
 * ============================================================ */
typedef enum {
    REG_R0  = 0,
    REG_R1,  REG_R2,  REG_R3,
    REG_R4,  REG_R5,
    REG_R6,           /* LOCKED: stack pointer */
    REG_R7,           /* LOCKED: frame pointer */
    REG_R8,  REG_R9,
    REG_R10, REG_R11, REG_R12,
    REG_R13,          /* T7: __state            */
    REG_R14,          /* T9: indirect target    */
    REG_R15,          /* T8: const decode tmp   */
    REG_COUNT
} reg_id_t;

#define REG_STATE  REG_R13
#define REG_ITGT   REG_R14
#define REG_CTMP   REG_R15

/* Legacy x86 name aliases */
#define REG_RAX REG_R0
#define REG_RBX REG_R1
#define REG_RCX REG_R2
#define REG_RDX REG_R3
#define REG_RSI REG_R4
#define REG_RDI REG_R5
#define REG_RSP REG_R6
#define REG_RBP REG_R7

/* ============================================================
 * CPU flags  (full set for accurate JCC simulation)
 * ============================================================ */
typedef struct { int zf, sf, cf, of, pf; } cpu_flags_t;

/* ============================================================
 * Operand
 * ============================================================ */
typedef struct {
    operand_type_t type;
    union {
        reg_id_t reg;
        int64_t  imm;
        struct { reg_id_t base; int64_t disp; } mem;
        char     label[CXS_LABEL_LEN];
        int      jtab_slot;
    };
} operand_t;

/* ============================================================
 * Instruction
 * ============================================================ */
typedef struct {
    insn_type_t  type;

    /* ── transform provenance flags ── */
    int  is_junk;      /* T2  : dead injection                           */
    int  is_opaque;    /* T4  : part of opaque predicate cluster         */
    int  is_overlap;   /* T6  : overlap byte sequence                    */
    int  is_flat;      /* T7  : part of CFF dispatch loop                */
    int  is_encoded;   /* T8  : immediate has been XOR/ADD/ROL encoded   */
    int  is_indirect;  /* T9  : jump has been replaced by IJMP           */
    int  is_dead;      /* T10 : belongs to unreachable dead-code block   */
    int  is_subst;     /* T11 : result of opcode substitution            */
    int  is_split;     /* T12 : part of data-flow split sequence         */
    int  is_stack;     /* T13 : fake stack noise instruction             */
    int  is_stub;      /* T14 : part of XOR decryption stub              */
    int  is_antiana;   /* T15 : anti-analysis marker                     */
    int  is_dataenc;   /* T16 : data-encrypted constant                  */
    int  is_bogus;     /* T17 : bogus argument injection                 */
    int  is_replicated;/* T18 : replicated neutral instruction           */
    int  is_virt;      /* T19 : inside CFG virtualization mini-VM        */
    int  is_alias;     /* T20 : alias register chain step                */
    int  is_loop;      /* T21 : fake loop structure instruction          */
    int  is_cfi;       /* T22 : frame marker (CFI/SEH)                   */
    int  is_ptrenc;    /* T23 : pointer-encoded address operand          */
    int  is_fakecp;    /* T24 : fake constant-propagation candidate      */
    int  is_outlined;  /* T25 : function outline noise                   */
    int  is_bitfield;  /* T26 : bitfield extraction noise                */
    int  is_chksum;    /* T27 : checksum guard step                      */
    int  is_cmov;      /* T28 : predicated-move obfuscation              */
    int  is_entropy;   /* T29 : entropy injection                        */
    int  is_keysched;  /* T30 : key schedule expansion step              */

    int  block_id;

    operand_t  ops[CXS_OPERAND_MAX];
    int        num_ops;

    char  label[CXS_LABEL_LEN];
    int   has_label;

    uint8_t  overlap_bytes[16];   /* T6: assembled pattern bytes         */
    int      overlap_len;
} insn_t;

/* ============================================================
 * Register renaming map  (T5)
 * ============================================================ */
typedef struct {
    reg_id_t map[REG_COUNT];
    int      locked[REG_COUNT];
} reg_map_t;

/* ============================================================
 * Jump table entry  (T9)
 * ============================================================ */
typedef struct {
    char     label[CXS_LABEL_LEN];
    int64_t  encode_key;
    int      used;
} jtab_entry_t;

/* ============================================================
 * Variable split record  (T12)
 * ============================================================ */
typedef struct {
    reg_id_t original;   /* the register being split          */
    reg_id_t hi;         /* split high half (original = hi^lo) */
    reg_id_t lo;         /* split low half                     */
    int64_t  mask;       /* initial XOR mask applied to hi     */
    int      active;
} split_rec_t;

/* ============================================================
 * Logical Block
 * ============================================================ */
typedef struct {
    int  id;
    int  start;
    int  end;
    int  next_id;
    int  state_id;             /* T7: assigned CFF state value   */
    int  is_dead;              /* T10: unreachable fake block     */
    char label[CXS_LABEL_LEN];
} block_t;

/* ============================================================
 * Alias register chain record  (T20)
 * ============================================================ */
typedef struct {
    reg_id_t original;   /* the register being aliased              */
    reg_id_t alias;      /* the alias register used in its place    */
    int      active;
} alias_rec_t;

/* ============================================================
 * Outlined stub record  (T25)
 * ============================================================ */
typedef struct {
    char  label[CXS_LABEL_LEN];   /* label of the outlined stub       */
    int   used;
} outline_rec_t;

/* ============================================================
 * Virtualization handler record  (T19)
 * ============================================================ */
typedef struct {
    char   label[CXS_LABEL_LEN];
    int    opcode;     /* virtual opcode byte 0-255                 */
    int    used;
} virt_handler_t;

/* ============================================================
 * Per-transform statistics
 * ============================================================ */
typedef struct {
    int arith_subs;         /* T1  */
    int junk_injected;      /* T2  */
    int blocks_reordered;   /* T3  */
    int opaque_inserted;    /* T4  */
    int regs_renamed;       /* T5  */
    int overlap_inserted;   /* T6  */
    int blocks_flattened;   /* T7  */
    int consts_encoded;     /* T8  */
    int jumps_indirected;   /* T9  */
    int dead_blocks;        /* T10 */
    int insns_substituted;  /* T11 */
    int vars_split;         /* T12 */
    int stack_noise;        /* T13 */
    int stub_insns;         /* T14 */
    int antiana_markers;    /* T15 */
    int data_encrypted;     /* T16 */
    int bogus_args;         /* T17 */
    int insns_replicated;   /* T18 */
    int virt_handlers;      /* T19 */
    int alias_chains;       /* T20 */
    int loop_noise;         /* T21 */
    int cfi_markers;        /* T22 */
    int ptrs_encoded;       /* T23 */
    int fakecp_chains;      /* T24 */
    int outlined_stubs;     /* T25 */
    int bitfield_seqs;      /* T26 */
    int chksum_guards;      /* T27 */
    int cmov_chains;        /* T28 */
    int entropy_insns;      /* T29 */
    int keysched_rounds;    /* T30 */
    int total_transforms;
} cxs_stats_t;

/* ============================================================
 * Engine state
 * ============================================================ */
typedef struct {
    insn_t       insns[CXS_MAX_INSN];
    int          num_insns;

    block_t      blocks[CXS_MAX_BLOCKS];
    int          num_blocks;

    int          exec_order[CXS_MAX_BLOCKS];
    int          num_exec_order;

    reg_map_t    rmap;

    jtab_entry_t jtab[CXS_MAX_JTAB];
    int          jtab_len;

    split_rec_t  splits[CXS_MAX_SPLIT];
    int          split_len;

    alias_rec_t  aliases[CXS_MAX_ALIAS];   /* T20 */
    int          alias_len;

    outline_rec_t outlines[CXS_MAX_OUTLINE]; /* T25 */
    int           outline_len;

    virt_handler_t vhandlers[CXS_MAX_HANDLERS]; /* T19 */
    int            vhandler_len;

    cxs_stats_t  stats;
    uint64_t     seed;

    int  opaque_serial;
    int  encode_serial;
    int  flat_done;
    int  stub_done;
    int  virt_done;      /* T19: virtualization applied          */
    int  keysched_done;  /* T30: key schedule applied            */
    int  outline_serial; /* T25: outline label counter           */
    int  loop_serial;    /* T21: loop label counter              */
    int  chksum_serial;  /* T27: checksum guard counter          */
    int  entropy_serial; /* T29: entropy label counter           */

    /* source file info (when obfuscating a target file) */
    char   source_file[256];
    int    has_source;
} cxs_engine_t;

/* ============================================================
 * Public API
 * ============================================================ */

/* Lifecycle */
void    cxs_engine_init(cxs_engine_t *e);
void    cxs_engine_free(cxs_engine_t *e);

/* Loaders */
void    cxs_load_sample_block(cxs_engine_t *e);
int     cxs_load_file(cxs_engine_t *e, const char *path);

/* Transforms */
void    cxs_transform_arithmetic(cxs_engine_t *e);     /* T1  */
void    cxs_inject_junk(cxs_engine_t *e);               /* T2  */
void    cxs_reorder_blocks(cxs_engine_t *e);            /* T3  */
void    cxs_insert_opaque_predicates(cxs_engine_t *e);  /* T4  */
void    cxs_rename_registers(cxs_engine_t *e);          /* T5  */
void    cxs_insert_overlap_bytes(cxs_engine_t *e);      /* T6  */
void    cxs_flatten_control_flow(cxs_engine_t *e);      /* T7  */
void    cxs_encode_constants(cxs_engine_t *e);          /* T8  */
void    cxs_indirect_control_flow(cxs_engine_t *e);     /* T9  */
void    cxs_insert_dead_code(cxs_engine_t *e);          /* T10 */
void    cxs_substitute_instructions(cxs_engine_t *e);   /* T11 */
void    cxs_obfuscate_data_flow(cxs_engine_t *e);       /* T12 */
void    cxs_mangle_stack_frame(cxs_engine_t *e);        /* T13 */
void    cxs_insert_decrypt_stub(cxs_engine_t *e);       /* T14 */
void    cxs_insert_antiana_markers(cxs_engine_t *e);    /* T15 */
void    cxs_encrypt_data_constants(cxs_engine_t *e);    /* T16 */
void    cxs_inject_bogus_args(cxs_engine_t *e);         /* T17 */
void    cxs_replicate_instructions(cxs_engine_t *e);    /* T18 */
void    cxs_virtualize_cfg(cxs_engine_t *e);            /* T19 */
void    cxs_alias_registers(cxs_engine_t *e);           /* T20 */
void    cxs_insert_loop_noise(cxs_engine_t *e);         /* T21 */
void    cxs_insert_cfi_noise(cxs_engine_t *e);          /* T22 */
void    cxs_obfuscate_pointers(cxs_engine_t *e);        /* T23 */
void    cxs_inject_fake_cprop(cxs_engine_t *e);         /* T24 */
void    cxs_outline_functions(cxs_engine_t *e);         /* T25 */
void    cxs_insert_bitfield_noise(cxs_engine_t *e);     /* T26 */
void    cxs_insert_checksum_guards(cxs_engine_t *e);    /* T27 */
void    cxs_obfuscate_moves(cxs_engine_t *e);           /* T28 */
void    cxs_inject_entropy(cxs_engine_t *e);            /* T29 */
void    cxs_insert_key_schedule(cxs_engine_t *e);       /* T30 */

/* Run the full 30-transform pipeline in recommended order */
void    cxs_run_pipeline(cxs_engine_t *e);

/* Execution & verification */
int64_t cxs_execute(cxs_engine_t *e, int64_t input);
void    cxs_capture_golden(cxs_engine_t *e);   /* call BEFORE pipeline */
int64_t cxs_golden_for(int64_t input);         /* lookup a golden value */
int     cxs_verify_equivalence(cxs_engine_t *e);

/* Display */
void    cxs_disasm_print(cxs_engine_t *e);
void    cxs_stats_print(cxs_engine_t *e);

/* Utilities */
int         cxs_rand_range(cxs_engine_t *e, int lo, int hi);
const char *cxs_reg_name(reg_id_t r);

/* ============================================================
 * T6 polyglot pattern symbols  (defined in arch ASM files,
 * read at runtime by cxs_insert_overlap_bytes)
 * ============================================================ */
#if defined(CXS_ARCH_X86_64) || defined(CXS_ARCH_ARM64)
extern uint8_t cxs_ovpat_p0[];  extern uint8_t cxs_ovpat_p0_end[];
extern uint8_t cxs_ovpat_p1[];  extern uint8_t cxs_ovpat_p1_end[];
extern uint8_t cxs_ovpat_p2[];  extern uint8_t cxs_ovpat_p2_end[];
extern uint8_t cxs_ovpat_p3[];  extern uint8_t cxs_ovpat_p3_end[];
extern uint8_t cxs_ovpat_p4[];  extern uint8_t cxs_ovpat_p4_end[];
#endif

#endif /* CXS_H */
