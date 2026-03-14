/* ===========================================================================
 * cxs_asm_x86_64.S  - CXS x86-64 ASM helpers + T6 polyglot patterns  v1.2
 *
 * GNU Assembler (GAS) AT&T syntax.
 * Assembled via: cc -c cxs_asm_x86_64.S  (no NASM required)
 *
 * T6 polyglot patterns:
 *   Each cxs_ovpat_pN function is a CPU-valid byte sequence containing a
 *   short-jump (EB xx) followed by dead bytes inside the gap.
 *   A linear-sweep disassembler decodes the dead bytes as instructions.
 *
 * The engine reads pattern bytes at runtime:
 *   len  = cxs_ovpat_pN_end - cxs_ovpat_pN   (linker-computed)
 *   copy = memcpy(buf, cxs_ovpat_pN, len)
 *
 * x86-64 SysV ABI: args rdi/rsi/rdx, return rax.
 * =========================================================================== */

    .text
    .align  16

/* =========================================================================
 * uint64_t cxs_xorshift64(uint64_t seed)
 * arg: %rdi    return: %rax
 * ========================================================================= */
    .global cxs_xorshift64
    .type   cxs_xorshift64, @function
cxs_xorshift64:
    movq    %rdi, %rax
    shlq    $13,  %rax
    xorq    %rax, %rdi          /* seed ^= seed << 13 */

    movq    %rdi, %rax
    shrq    $7,   %rax
    xorq    %rax, %rdi          /* seed ^= seed >> 7  */

    movq    %rdi, %rax
    shlq    $17,  %rax
    xorq    %rax, %rdi          /* seed ^= seed << 17 */

    movq    %rdi, %rax
    ret
    .size   cxs_xorshift64, .-cxs_xorshift64

/* =========================================================================
 * void cxs_memcpy_volatile(void *dst, const void *src, size_t n)
 * args: %rdi=dst  %rsi=src  %rdx=n
 * ========================================================================= */
    .global cxs_memcpy_volatile
    .type   cxs_memcpy_volatile, @function
cxs_memcpy_volatile:
    testq   %rdx, %rdx
    jz      .Lmv_done
.Lmv_loop:
    movb    (%rsi), %al
    movb    %al, (%rdi)
    incq    %rsi
    incq    %rdi
    decq    %rdx
    jnz     .Lmv_loop
.Lmv_done:
    ret
    .size   cxs_memcpy_volatile, .-cxs_memcpy_volatile

/* =========================================================================
 * int64_t cxs_golden_fn(int64_t x)
 * f(x) = (x+5-3)*2    arg: %rdi    return: %rax
 * ========================================================================= */
    .global cxs_golden_fn
    .type   cxs_golden_fn, @function
cxs_golden_fn:
    movq    %rdi, %rax
    addq    $5,   %rax
    subq    $3,   %rax
    imulq   $2,   %rax
    ret
    .size   cxs_golden_fn, .-cxs_golden_fn

/* =========================================================================
 * int64_t cxs_add5_sub3(int64_t x)
 * ========================================================================= */
    .global cxs_add5_sub3
    .type   cxs_add5_sub3, @function
cxs_add5_sub3:
    movq    %rdi, %rax
    addq    $5,   %rax
    subq    $3,   %rax
    ret
    .size   cxs_add5_sub3, .-cxs_add5_sub3

/* =========================================================================
 * void cxs_nop_sled_8(void)
 * ========================================================================= */
    .global cxs_nop_sled_8
    .type   cxs_nop_sled_8, @function
cxs_nop_sled_8:
    nop; nop; nop; nop
    nop; nop; nop; nop
    ret
    .size   cxs_nop_sled_8, .-cxs_nop_sled_8

/* =========================================================================
 * void cxs_poly_marker(void)
 * 0x43='C'  0x58='X'  0x53='S' as actual instruction bytes:
 *   REX.X (0x43) + POP %rax (0x58) + PUSH %rbx (0x53) — harmless sequence.
 * ========================================================================= */
    .global cxs_poly_marker
    .type   cxs_poly_marker, @function
cxs_poly_marker:
    .byte   0x43, 0x58, 0x53    /* REX.X  POP %rax  PUSH %rbx  => "CXS" */
    nop
    ret
    .size   cxs_poly_marker, .-cxs_poly_marker

/* =========================================================================
 * T6 Polyglot overlap patterns — assembled by GAS, read at runtime
 *
 * CPU: follows the JMP SHORT, executes nop/ret after label, correct.
 * Linear-sweep disasm: also decodes the dead bytes inside the jump gap.
 *
 * Pattern bytes copied by engine: [cxs_ovpat_pN .. cxs_ovpat_pN_end)
 * The _end label sits immediately before the trailing nop/ret so that
 * the engine only copies the polyglot body, not the pad bytes after.
 * ========================================================================= */

/* --- P0: JMP SHORT +3 / shadow near-CALL --------------------------------
 * CPU:    jmp → .p0r        (skips 3 dead bytes)
 * Linear: also decodes E8 FF FF = start of CALL -1  (near-call to -1)
 *                       00      = ADD (%rax),%al
 * Bytes in pattern: EB 03 E8 FF FF 00
 */
    .global cxs_ovpat_p0
    .global cxs_ovpat_p0_end
    .type   cxs_ovpat_p0, @function
cxs_ovpat_p0:
    jmp     .Lp0r               /* EB 03 */
    .byte   0xE8, 0xFF, 0xFF, 0x00  /* dead: CALL -1 bytes (never executed) */
.Lp0r:
cxs_ovpat_p0_end:
    nop
    ret
    .size   cxs_ovpat_p0, cxs_ovpat_p0_end - cxs_ovpat_p0

/* --- P1: JMP SHORT +4 / ENDBR64 ghost ----------------------------------
 * CPU:    jmp → .p1r        (skips 4 dead bytes)
 * Linear: also decodes F3 0F 1E FA = ENDBR64  (CET end-branch marker)
 * Confuses CFI checkers and call-graph reconstruction tools.
 * Bytes in pattern: EB 04 F3 0F 1E FA
 */
    .global cxs_ovpat_p1
    .global cxs_ovpat_p1_end
    .type   cxs_ovpat_p1, @function
cxs_ovpat_p1:
    jmp     .Lp1r               /* EB 04 */
    .byte   0xF3, 0x0F, 0x1E, 0xFA  /* dead: ENDBR64 (never executed) */
.Lp1r:
cxs_ovpat_p1_end:
    nop
    ret
    .size   cxs_ovpat_p1, cxs_ovpat_p1_end - cxs_ovpat_p1

/* --- P2: JMP SHORT +2 / JMP RAX shadow ---------------------------------
 * CPU:    jmp → .p2r        (skips 2 dead bytes)
 * Linear: also decodes FF E0 = JMP *%rax  (indirect jump — alarming!)
 * Bytes in pattern: EB 02 FF E0
 */
    .global cxs_ovpat_p2
    .global cxs_ovpat_p2_end
    .type   cxs_ovpat_p2, @function
cxs_ovpat_p2:
    jmp     .Lp2r               /* EB 02 */
    .byte   0xFF, 0xE0          /* dead: JMP *%rax (never executed) */
.Lp2r:
cxs_ovpat_p2_end:
    nop
    ret
    .size   cxs_ovpat_p2, cxs_ovpat_p2_end - cxs_ovpat_p2

/* --- P3: JMP SHORT +3 / multi-prefix NOP shadow -------------------------
 * CPU:    jmp → .p3r        (skips 3 dead bytes)
 * Linear: also decodes 66 0F 1F = start of multi-byte NOP w/ op-size prefix
 * Looks like benign padding to a static analyzer.
 * Bytes in pattern: EB 03 66 0F 1F
 */
    .global cxs_ovpat_p3
    .global cxs_ovpat_p3_end
    .type   cxs_ovpat_p3, @function
cxs_ovpat_p3:
    jmp     .Lp3r               /* EB 03 */
    .byte   0x66, 0x0F, 0x1F    /* dead: multi-byte NOP prefix (never executed) */
.Lp3r:
cxs_ovpat_p3_end:
    nop
    ret
    .size   cxs_ovpat_p3, cxs_ovpat_p3_end - cxs_ovpat_p3

/* --- P4: JMP SHORT +5 / JMP32 DEADBEEF ghost ----------------------------
 * CPU:    jmp → .p4r        (skips 5 dead bytes)
 * Linear: also decodes E9 DE AD BE EF = JMP 0xEFBEADDE  (DEADBEEF bait!)
 * Classic analyst bait — looks like intentional obfuscation marker.
 * Bytes in pattern: EB 05 E9 DE AD BE EF
 */
    .global cxs_ovpat_p4
    .global cxs_ovpat_p4_end
    .type   cxs_ovpat_p4, @function
cxs_ovpat_p4:
    jmp     .Lp4r               /* EB 05 */
    .byte   0xE9, 0xDE, 0xAD, 0xBE, 0xEF  /* dead: JMP DEADBEEF (never executed) */
.Lp4r:
cxs_ovpat_p4_end:
    nop
    ret
    .size   cxs_ovpat_p4, cxs_ovpat_p4_end - cxs_ovpat_p4
    .section .note.GNU-stack,"",@progbits
