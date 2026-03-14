/*
 * cxs_emit.h  —  CXS Assembly Emitter  v1.0
 * ============================================================
 * Emits the obfuscated IR to a real assembly source file.
 *
 * Supported targets (auto-detected from CXS_ARCH_* or --target):
 *
 *   CXS_EMIT_X86_64   — GAS AT&T syntax  (.S)
 *                        Linux / macOS / BSD
 *   CXS_EMIT_ARM64    — GAS AArch64 syntax  (.S)
 *                        Android / iOS / Apple Silicon
 *
 * Output file naming:
 *   --emit-asm                  obfuscated_x86_64.S   (auto-detect arch)
 *   --emit-asm -o myfile.S      myfile.S
 *
 * The emitted file is a valid, self-contained .S that can be
 * assembled with:
 *   as -o out.o obfuscated_x86_64.S   (GAS)
 *   cc -c -o out.o obfuscated_x86_64.S
 *
 * File structure:
 *   .section .text
 *   .global cxs_fn
 *
 *   cxs_fn:           ; T14 decrypt stub (if present)
 *     <XOR_STUB insns>
 *   .decrypt_stub:
 *     ...
 *   .cff_loop:        ; T7 dispatcher (if CFF applied)
 *     cmp  r13, 0
 *     je   .cff_b0
 *     ...
 *   .cff_b0:
 *     add  rax, rax
 *     ...
 *   .cff_done:
 *     ret
 *
 * Decoration:
 *   Every emitted instruction has an inline comment showing
 *   which transform injected it:
 *     add  rax, rax    ; [T1] arithmetic substitution
 *     nop              ; [T2] junk
 *     test r10, 0      ; [T4] opaque predicate
 *     cpuid            ; [T15] anti-analysis trap
 * ============================================================
 */

#ifndef CXS_EMIT_H
#define CXS_EMIT_H

#include <stdio.h>
#include "cxs.h"

/* Emit target */
typedef enum {
    CXS_EMIT_NATIVE = 0,   /* auto-detect from CXS_ARCH_*         */
    CXS_EMIT_X86_64,       /* force x86-64 GAS output             */
    CXS_EMIT_ARM64,        /* force AArch64 GAS output            */
} cxs_emit_target_t;

/* Emit options */
typedef struct {
    cxs_emit_target_t target;
    const char       *outfile;    /* NULL = auto-name                */
    const char       *fn_name;    /* exported function symbol name   */
    int               annotate;   /* 1 = inline transform comments   */
    int               show_stats; /* 1 = prepend stats block comment */
} cxs_emit_opts_t;

/*
 * cxs_emit_asm()
 *
 * Emit the current engine state as assembly source.
 * Returns CXS_OK on success, CXS_ERR on I/O failure.
 *
 * Call AFTER cxs_run_pipeline() — emits the post-transform IR.
 */
int cxs_emit_asm(cxs_engine_t *e, const cxs_emit_opts_t *opts);

/* Default options (annotated, auto-named, fn=cxs_fn) */
void cxs_emit_opts_default(cxs_emit_opts_t *opts);

/* Return the auto-generated output filename for a given target */
const char *cxs_emit_default_filename(cxs_emit_target_t target);

#endif /* CXS_EMIT_H */
