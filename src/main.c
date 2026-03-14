/*
 * main.c  —  CXS Command-Line Interface  v4.0
 * ============================================================
 *
 * USAGE
 * ─────
 *   cxs                          interactive demo (built-in sample)
 *   cxs --demo                   same as above
 *   cxs -f <file.cxs>            obfuscate a target IR file
 *   cxs -f <file.cxs> -s N       obfuscate file and run N verify cycles
 *   cxs --stress N               stress-test built-in sample N cycles
 *   cxs --help                   print this help
 *   cxs --version                print version info
 *
 * FILE MODE (-f)
 * ──────────────
 *   Reads a .cxs IR text file, applies all 30 transform passes,
 *   prints the disassembly before and after, then verifies semantic
 *   equivalence.  Use -s N to repeat the cycle N times to check
 *   that the engine is stable across multiple runs.
 *
 * CXS IR FILE FORMAT
 * ──────────────────
 *   # comment
 *   .block .label_name
 *     <mnemonic>  [dst [, src]]
 *   .end
 *
 *   Registers: rax rbx … r15  (x86) | x0…x15 sp fp (AArch64)
 *   Immediates: 42 / -7 / 0xFF
 *   Labels: .name (start with a dot)
 *
 * EXAMPLE
 * ───────
 *   cxs -f samples/double.cxs
 *   cxs -f samples/double.cxs -s 20
 *   cxs --stress 50
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cxs.h"
#include "cxs_emit.h"

/* ============================================================
 * Banner
 * ============================================================ */
static void print_banner(void) {
    printf("\n");
    printf("  ██████╗██╗  ██╗███████╗\n");
    printf(" ██╔════╝╚██╗██╔╝██╔════╝\n");
    printf(" ██║      ╚███╔╝ ███████╗\n");
    printf(" ██║      ██╔██╗ ╚════██║\n");
    printf(" ╚██████╗██╔╝ ██╗███████║\n");
    printf("  ╚═════╝╚═╝  ╚═╝╚══════╝\n");
    printf("\n");
    printf("  Code eXecution Scrambler  v4.0\n");
    printf("  Polymorphic Self-Modifying ASM Engine\n");
    printf("  Platform : Linux");
#if defined(CXS_ARCH_X86_64)
    printf("  |  Arch : x86-64");
#elif defined(CXS_ARCH_ARM64)
    printf("  |  Arch : AArch64");
#else
    printf("  |  Arch : generic");
#endif
    printf("  |  Transforms : T1–T30\n");
    printf("\n");
}

/* ============================================================
 * Help / version
 * ============================================================ */
static void print_help(const char *prog) {
    printf("USAGE\n");
    printf("  %s [options]\n\n", prog);
    printf("OPTIONS\n");
    printf("  (no args)              Run interactive demo on built-in sample program\n");
    printf("  --demo                 Same as above\n");
    printf("  -f <file.cxs>          Obfuscate a target IR file\n");
    printf("  -f <file.cxs> -s N     Obfuscate file and run N stress-test cycles\n");
    printf("  --stress N             Stress-test built-in sample for N cycles\n");
    printf("  --help  / -h           Show this help message\n");
    printf("  --version              Show version and build info\n\n");

    printf("TRANSFORM PIPELINE  (T1 – T30)\n");
    printf("  Layer 1 — Instruction level\n");
    printf("    T1   Arithmetic substitution     ADD<->SUB, INC->ADD 1, IMUL->SHL\n");
    printf("    T2   Junk injection              dead NOP/XOR/AND sleds\n");
    printf("    T6   Instruction overlap bytes   polyglot byte patterns from real ASM\n");
    printf("    T11  Instruction substitution    semantic-equivalent opcode swaps\n");
    printf("    T18  Instruction replication     semantically-neutral instruction clones\n");
    printf("    T26  Bitfield extraction noise   fake BSWAP/ROR/shift sequences\n");
    printf("  Layer 2 — Data / operand\n");
    printf("    T5   Register renaming           Fisher-Yates scratch-register rotation\n");
    printf("    T8   Constant encoding           XOR/ADD/ROL obfuscation of immediates\n");
    printf("    T12  Data flow obfuscation       variable splitting across register pairs\n");
    printf("    T13  Stack frame mangling        fake push/pop + ESP arithmetic noise\n");
    printf("    T16  Data encryption             rolling-XOR encryption of inline constants\n");
    printf("    T20  Alias register chains       r_alias=orig -> use alias -> restore\n");
    printf("    T23  Pointer obfuscation         base+encoded_offset address encoding\n");
    printf("    T24  Constant propagation fake   fake CProp candidate chains (XOR poison)\n");
    printf("    T28  Predicated move obfuscation MOV -> CMOV ladder requiring flag analysis\n");
    printf("  Layer 3 — Control flow\n");
    printf("    T3   Block reordering            shuffle basic blocks + JMP glue\n");
    printf("    T4   Opaque predicates           always-false branches (5 math patterns)\n");
    printf("    T7   Control flow flattening     state-machine dispatch loop (CFF)\n");
    printf("    T9   Indirect control flow       jump table + XOR-encoded targets\n");
    printf("    T10  Dead code insertion         unreachable fake function blocks\n");
    printf("    T17  Bogus function arguments    fake arg-setup MOV sequences\n");
    printf("    T19  CFG virtualization lite     mini-VM bytecode handler dispatch\n");
    printf("    T21  Loop unrolling noise        fake counted loop (always 1 iteration)\n");
    printf("    T25  Function outline noise      fake CALL+RET helper outline stubs\n");
    printf("  Layer 4 — Anti-analysis\n");
    printf("    T14  Polymorphic decrypt stub    XOR self-modifying prologue\n");
    printf("    T15  Anti-analysis markers       CPUID/RDTSC traps + fake branches\n");
    printf("    T22  Exception frame noise       fake .cfi_startproc / SEH markers\n");
    printf("    T27  Checksum guards             rolling-XOR integrity check stubs\n");
    printf("    T29  Entropy injection           RDRAND/PRNG-seed sequences\n");
    printf("    T30  Multi-layer key schedule    multi-round XOR-ADD-ROL expansion\n\n");

    printf("IR FILE FORMAT  (.cxs)\n");
    printf("  # comment line\n");
    printf("  .block .entry\n");
    printf("    label  .entry\n");
    printf("    add    rax, 5\n");
    printf("    jmp    .done\n");
    printf("  .end\n");
    printf("  .block .done\n");
    printf("    label  .done\n");
    printf("    ret\n");
    printf("  .end\n\n");

    printf("EMIT OPTIONS\n");
    printf("  --emit-asm             Emit obfuscated assembly to .S file (auto-named)\n");
    printf("  --emit-asm -o out.S    Emit to custom filename\n");
    printf("  --emit-asm --x86-64    Force x86-64 GAS AT&T output\n");
    printf("  --emit-asm --arm64     Force AArch64 GAS output\n\n");
    printf("EXAMPLES\n");
    printf("  %s                                  # run built-in demo\n", prog);
    printf("  %s --stress 50                      # 50-cycle stress test (T1-T30)\n", prog);
    printf("  %s -f samples/double.cxs            # obfuscate file\n", prog);
    printf("  %s -f samples/double.cxs -s 20      # obfuscate + 20 verify cycles\n", prog);
    printf("  %s -f samples/double.cxs --emit-asm           # emit native ASM\n", prog);
    printf("  %s -f samples/double.cxs --emit-asm --arm64   # force AArch64 output\n", prog);
    printf("  %s -f samples/double.cxs --emit-asm -o out.S  # custom output file\n\n", prog);
}

static void print_version(void) {
    printf("CXS — Code eXecution Scrambler\n");
    printf("  Version   : 4.0\n");
    printf("  Transforms: T1–T30 (30 passes)\n");
    printf("  Built     : %s %s\n", __DATE__, __TIME__);
#if defined(CXS_ARCH_X86_64)
    printf("  Target    : x86-64\n");
#elif defined(CXS_ARCH_ARM64)
    printf("  Target    : AArch64\n");
#else
    printf("  Target    : generic\n");
#endif
}

/* ============================================================
 * Progress bar helper
 * ============================================================ */
static void progress_bar(int cur, int total, const char *label) {
    const int width = 30;
    int filled = (total > 0) ? (cur * width / total) : 0;
    printf("\r  [");
    for (int i = 0; i < width; i++) printf(i < filled ? "#" : ".");
    printf("] %3d/%3d  %s   ", cur, total, label);
    fflush(stdout);
    if (cur == total) printf("\n");
}

/* ============================================================
 * Section separator
 * ============================================================ */
static void section(const char *title) {
    printf("\n");
    printf("  ╔══════════════════════════════════════════════════╗\n");
    printf("  ║  %-48s║\n", title);
    printf("  ╚══════════════════════════════════════════════════╝\n");
}

/* ============================================================
 * Run one complete obfuscation + verify cycle on engine `e`
 *
 * Returns CXS_OK if all verify vectors pass, CXS_ERR otherwise.
 * ============================================================ */
typedef struct {
    int         mode;       /* 0=demo, 1=file, 2=stress */
    const char *file;
    int         stress_n;
    int         show_help;
    int         show_version;
    /* emit-asm options */
    int         do_emit;
    const char *emit_out;       /* -o <file> */
    int         emit_target;    /* 0=native, 1=x86-64, 2=arm64 */
} cli_args_t;

#define MODE_DEMO   0
#define MODE_FILE   1
#define MODE_STRESS 2

static int run_obfuscate_and_verify(cxs_engine_t *e, int show_disasm,
                                    const cli_args_t *args) {
    /* ── BEFORE ── */
    if (show_disasm) {
        section("ORIGINAL IR  (before any transforms)");
        cxs_disasm_print(e);
    }

    /* ── Transform pipeline ── */
    section("APPLYING TRANSFORMS  (T1 – T30)");
    clock_t t_start = clock();
        cxs_capture_golden(e);
    cxs_run_pipeline(e);
    double elapsed = (double)(clock()-t_start)/CLOCKS_PER_SEC;
    printf("  Pipeline wall-time: %.3f ms\n", elapsed*1000.0);

    /* ── AFTER ── */
    if (show_disasm) {
        section("OBFUSCATED IR  (after all 30 transforms)");
        cxs_disasm_print(e);
    }

    /* ── Statistics ── */
    section("TRANSFORM STATISTICS");
    cxs_stats_print(e);

    /* ── Verification ── */
    section("SEMANTIC EQUIVALENCE VERIFICATION");
    printf("  Testing %d input vectors against pre-transform golden snapshot\n\n", 32);
    clock_t tv_start = clock();
    int ok = cxs_verify_equivalence(e);
    double tv_elapsed = (double)(clock()-tv_start)/CLOCKS_PER_SEC;

    if (ok == CXS_OK) {
        printf("\n  [PASS]  All vectors matched.  (%.3f ms)\n", tv_elapsed*1000.0);
    } else {
        printf("\n  [FAIL]  Semantic mismatch detected!\n");
    }

    /* ── Spot execution ── */
    printf("\n  Spot check: f(10) = %"PRId64"  (expected %"PRId64")\n",
           cxs_execute(e, 10), cxs_golden_for(10));

    /* ── Assembly emit (when --emit-asm requested) ── */
    if (args && args->do_emit) {
        section("EMITTING ASSEMBLY OUTPUT");
        cxs_emit_opts_t eopts;
        cxs_emit_opts_default(&eopts);
        eopts.target  = (cxs_emit_target_t)args->emit_target;
        eopts.outfile = args->emit_out;
        cxs_emit_asm(e, &eopts);
    }

    return ok;
}

/* ============================================================
 * Interactive demo mode  (built-in sample program)
 * ============================================================ */

static void run_demo(const cli_args_t *args) {
    section("DEMO MODE — built-in sample: f(x) = (x+5-3)*2");

    cxs_engine_t *e = malloc(sizeof(cxs_engine_t));
    if (!e) { fprintf(stderr, "  [ERROR] Out of memory\n"); return; }

    printf("\n  Initializing engine...\n");
    cxs_engine_init(e);

    printf("  Loading built-in sample program...\n");
    cxs_load_sample_block(e);

    run_obfuscate_and_verify(e, /*show_disasm=*/1, args);

    cxs_engine_free(e);
    free(e);
}

/* ============================================================
 * File obfuscation mode  (-f <file>)
 * ============================================================ */
static void run_file(const char *path, int stress_n, const cli_args_t *args) {
    section("FILE OBFUSCATION MODE");
    printf("\n  Target file : %s\n", path);
    if (stress_n > 1)
        printf("  Stress cycles: %d\n", stress_n);
    printf("\n");

    cxs_engine_t *e = malloc(sizeof(cxs_engine_t));
    if (!e) { fprintf(stderr, "  [ERROR] Out of memory\n"); return; }

    /* Load once and show the original IR */
    cxs_engine_init(e);
    int rc = cxs_load_file(e, path);
    if (rc != CXS_OK) {
        fprintf(stderr, "  [ERROR] Failed to load file: %s\n", path);
        free(e); return;
    }

    if (stress_n <= 1) {
        /* Single run with full disasm output */
        run_obfuscate_and_verify(e, /*show_disasm=*/1, args);
    } else {
        /* Multi-cycle stress test — reload each iteration */
        section("STRESS TEST — file mode");
        printf("  Running %d obfuscation + verify cycles on: %s\n\n",
               stress_n, path);

        int passed = 0;
        for (int i = 1; i <= stress_n; i++) {
            cxs_engine_init(e);
            rc = cxs_load_file(e, path);
            if (rc != CXS_OK) break;

            cxs_capture_golden(e);
            cxs_run_pipeline(e);
            int ok = cxs_verify_equivalence(e);
            if (ok == CXS_OK) passed++;

            progress_bar(i, stress_n, ok==CXS_OK ? "PASS" : "FAIL");
        }

        printf("\n");
        section("STRESS TEST RESULTS");
        printf("  File    : %s\n", path);
        printf("  Cycles  : %d\n", stress_n);
        printf("  Passed  : %d\n", passed);
        printf("  Failed  : %d\n", stress_n - passed);
        printf("  Result  : %s\n",
               passed==stress_n ? "[ALL PASS]" : "[FAILURES DETECTED]");
    }

    cxs_engine_free(e);
    free(e);
}

/* ============================================================
 * Stress test mode  (built-in sample, --stress N)
 * ============================================================ */
static void run_stress(int n) {
    section("STRESS TEST MODE — built-in sample");
    printf("\n  Running %d obfuscation + verify cycles...\n\n", n);

    int passed = 0;
    for (int i = 1; i <= n; i++) {
        cxs_engine_t *e = malloc(sizeof(cxs_engine_t));
        if (!e) break;

        cxs_engine_init(e);
        cxs_load_sample_block(e);
                cxs_capture_golden(e);
        cxs_run_pipeline(e);

        int ok = cxs_verify_equivalence(e);
        if (ok == CXS_OK) passed++;

        progress_bar(i, n, ok==CXS_OK ? "PASS" : "FAIL");

        cxs_engine_free(e);
        free(e);
    }

    printf("\n");
    section("STRESS TEST RESULTS");
    printf("  Cycles  : %d\n", n);
    printf("  Passed  : %d\n", passed);
    printf("  Failed  : %d\n", n - passed);
    printf("  Result  : %s\n\n",
           passed==n ? "[ALL PASS]" : "[FAILURES DETECTED]");
}

/* ============================================================
 * Argument parser
 * ============================================================ */


static cli_args_t parse_args(int argc, char **argv) {
    cli_args_t a; memset(&a, 0, sizeof(a)); a.mode = MODE_DEMO; a.stress_n = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            a.show_help = 1;

        } else if (strcmp(argv[i], "--version") == 0) {
            a.show_version = 1;

        } else if (strcmp(argv[i], "--demo") == 0) {
            a.mode = MODE_DEMO;

        } else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0)
                   && i+1 < argc) {
            a.mode = MODE_FILE;
            a.file = argv[++i];

        } else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stress") == 0)
                   && i+1 < argc) {
            int n = atoi(argv[++i]);
            if (n > 0) {
                a.stress_n = n;
                if (a.mode != MODE_FILE) a.mode = MODE_STRESS;
            }
        } else if (strcmp(argv[i], "--stress") == 0 && i+1 < argc) {
            /* long form handled above */
        } else if (strcmp(argv[i], "--emit-asm") == 0) {
            a.do_emit = 1;

        } else if (strcmp(argv[i], "--x86-64") == 0 ||
                   strcmp(argv[i], "--x86_64") == 0) {
            a.emit_target = CXS_EMIT_X86_64;

        } else if (strcmp(argv[i], "--arm64") == 0 ||
                   strcmp(argv[i], "--aarch64") == 0) {
            a.emit_target = CXS_EMIT_ARM64;

        } else if ((strcmp(argv[i], "-o") == 0 ||
                    strcmp(argv[i], "--output") == 0) && i+1 < argc) {
            a.emit_out = argv[++i];

        } else {
            /* bare number = stress count (legacy compat) */
            int n = atoi(argv[i]);
            if (n > 0 && a.mode != MODE_FILE) {
                a.mode     = MODE_STRESS;
                a.stress_n = n;
            }
        }
    }
    return a;
}

/* ============================================================
 * main
 * ============================================================ */
int main(int argc, char **argv) {
    print_banner();

    cli_args_t args = parse_args(argc, argv);

    if (args.show_version) { print_version(); return 0; }
    if (args.show_help)    { print_help(argv[0]); return 0; }

    switch (args.mode) {
        case MODE_DEMO:
            run_demo(&args);
            break;

        case MODE_FILE:
            if (!args.file) {
                fprintf(stderr, "  [ERROR] -f requires a filename.\n");
                print_help(argv[0]);
                return 1;
            }
            run_file(args.file, args.stress_n, &args);
            break;

        case MODE_STRESS:
            run_stress(args.stress_n);
            break;

        default:
            run_demo(&args);
            break;
    }

    return 0;
}
