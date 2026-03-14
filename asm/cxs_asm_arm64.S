// =============================================================================
// cxs_asm_arm64.S  - CXS AArch64 ASM helpers + T6 polyglot patterns  v1.2
//
// Assembled by GCC/Clang as preprocessed assembly (.S).
//
// T6 polyglot patterns — each cxs_ovpat_pN function contains a real
// B instruction that the CPU executes correctly, followed by dead
// instructions inside the gap.  A naive linear-sweep disassembler also
// decodes the dead instructions, producing different disassembly.
//
// The engine reads bytes at runtime via:
//   extern uint8_t cxs_ovpat_pN[];
//   extern uint8_t cxs_ovpat_pN_end[];
//   len  = cxs_ovpat_pN_end - cxs_ovpat_pN
//   memcpy(buf, cxs_ovpat_pN, len)
//
// AArch64 instruction facts used here (all 4 bytes LE):
//   NOP      = HINT #0 = 0xD503201F  -> bytes: 1F 20 03 D5
//   BRK #0   = 0xD4200000            -> bytes: 00 00 20 D4
//   MOV x0,x0= ORR x0,xzr,x0 = 0xAA0003E0 -> bytes: E0 03 00 AA
//   MOVZ x0,#0 = 0xD2800000          -> bytes: 00 00 80 D2
//   YIELD    = HINT #1 = 0xD5033F1F  -> bytes: 3F 20 03 D5
//   B +N insns = 0x14000000|N        -> B+2: 02 00 00 14
//
// AArch64 AAPCS64 calling convention:
//   Args: x0..x7    Return: x0
//   Caller-saved: x9..x15   Callee-saved: x19..x28, fp, lr
// =============================================================================

    .section    .text
    .align      4

// =============================================================================
// uint64_t cxs_xorshift64(uint64_t seed)
// Arg: x0=seed   Return: x0
// =============================================================================
    .global cxs_xorshift64
    .type   cxs_xorshift64, %function
cxs_xorshift64:
    lsl     x1, x0, #13
    eor     x0, x0, x1
    lsr     x1, x0, #7
    eor     x0, x0, x1
    lsl     x1, x0, #17
    eor     x0, x0, x1
    ret
    .size   cxs_xorshift64, .-cxs_xorshift64

// =============================================================================
// void cxs_memcpy_volatile(void *dst, const void *src, size_t n)
// Args: x0=dst, x1=src, x2=n
// =============================================================================
    .global cxs_memcpy_volatile
    .type   cxs_memcpy_volatile, %function
cxs_memcpy_volatile:
    cbz     x2, .Lmv_done
.Lmv_loop:
    ldrb    w3, [x1], #1
    strb    w3, [x0], #1
    subs    x2, x2, #1
    bne     .Lmv_loop
.Lmv_done:
    ret
    .size   cxs_memcpy_volatile, .-cxs_memcpy_volatile

// =============================================================================
// int64_t cxs_golden_fn(int64_t x)
// f(x) = (x+5-3)*2    Arg: x0   Return: x0
// =============================================================================
    .global cxs_golden_fn
    .type   cxs_golden_fn, %function
cxs_golden_fn:
    add     x0, x0, #5
    sub     x0, x0, #3
    lsl     x0, x0, #1
    ret
    .size   cxs_golden_fn, .-cxs_golden_fn

// =============================================================================
// int64_t cxs_add5_sub3(int64_t x)
// =============================================================================
    .global cxs_add5_sub3
    .type   cxs_add5_sub3, %function
cxs_add5_sub3:
    add     x0, x0, #5
    sub     x0, x0, #3
    ret
    .size   cxs_add5_sub3, .-cxs_add5_sub3

// =============================================================================
// void cxs_nop_sled_8(void)
// =============================================================================
    .global cxs_nop_sled_8
    .type   cxs_nop_sled_8, %function
cxs_nop_sled_8:
    nop; nop; nop; nop
    nop; nop; nop; nop
    ret
    .size   cxs_nop_sled_8, .-cxs_nop_sled_8

// =============================================================================
// void cxs_poly_marker(void)
// ASCII 'C','X','S' = 0x43,0x58,0x53 embedded as unreachable data after ret.
// On ARM64 these bytes are in UNALLOCATED encoding space — CPU would fault
// if it reached them. Only used as a searchable signature by the engine.
// =============================================================================
    .global cxs_poly_marker
    .type   cxs_poly_marker, %function
cxs_poly_marker:
    nop
    ret
    .byte   0x43, 0x58, 0x53, 0x00   // 'CXS\0' — unreachable signature
    .size   cxs_poly_marker, .-cxs_poly_marker

// =============================================================================
// T6 Polyglot overlap patterns — assembled by GAS, read at runtime
//
// Structure of each pattern:
//   [B +N]  [N dead instructions — valid AArch64, never executed]  [ret]
//
// CPU: follows the B, executes ret, done.
// Linear-sweep disassembler: also decodes the N dead instructions.
//
// The engine copies bytes [cxs_ovpat_pN .. cxs_ovpat_pN_end) at runtime.
// _end labels are .Lp*_end (local), size computed via .size directive.
// We also export the _end symbol directly for C linkage.
// =============================================================================

// --- P0: B+2 / dead NOP ---------------------------------------------------
// CPU:    B skips 1 instruction (4 bytes), lands on ret
// Linear: also decodes NOP (0xD503201F)
// Assembled bytes: 02 00 00 14 | 1F 20 03 D5 | (ret follows but not copied)
    .global cxs_ovpat_p0
    .global cxs_ovpat_p0_end
    .type   cxs_ovpat_p0, %function
cxs_ovpat_p0:
    b       .Lp0_resume         // B +2 insns (skips 1 dead insn = 4 bytes)
    nop                         // dead NOP: 1F 20 03 D5
.Lp0_resume:
cxs_ovpat_p0_end:
    ret
    .size   cxs_ovpat_p0, cxs_ovpat_p0_end - cxs_ovpat_p0

// --- P1: B+3 / dead BRK#0 + NOP ------------------------------------------
// CPU:    B skips 2 instructions (8 bytes), lands on ret
// Linear: also decodes BRK #0 (0xD4200000) then NOP (0xD503201F)
// BRK #0 looks alarming — "why is there a breakpoint in this code?"
// Assembled bytes: 03 00 00 14 | 00 00 20 D4 | 1F 20 03 D5
    .global cxs_ovpat_p1
    .global cxs_ovpat_p1_end
    .type   cxs_ovpat_p1, %function
cxs_ovpat_p1:
    b       .Lp1_resume         // B +3 insns (skips 2 dead insns = 8 bytes)
    brk     #0                  // dead BRK#0: 00 00 20 D4
    nop                         // dead NOP:   1F 20 03 D5
.Lp1_resume:
cxs_ovpat_p1_end:
    ret
    .size   cxs_ovpat_p1, cxs_ovpat_p1_end - cxs_ovpat_p1

// --- P2: B+2 / dead MOVZ x0, #0 ------------------------------------------
// CPU:    B skips 1 instruction (4 bytes), lands on ret
// Linear: also decodes MOVZ x0, #0 (0xD2800000)
// Looks like "clearing return value before real code" — misleads analyst
// Assembled bytes: 02 00 00 14 | 00 00 80 D2
    .global cxs_ovpat_p2
    .global cxs_ovpat_p2_end
    .type   cxs_ovpat_p2, %function
cxs_ovpat_p2:
    b       .Lp2_resume         // B +2 insns
    movz    x0, #0              // dead MOVZ x0,#0: 00 00 80 D2
.Lp2_resume:
cxs_ovpat_p2_end:
    ret
    .size   cxs_ovpat_p2, cxs_ovpat_p2_end - cxs_ovpat_p2

// --- P3: B+4 / dead BRK#0 + MOV x0,x0 + NOP ------------------------------
// CPU:    B skips 3 instructions (12 bytes), lands on ret
// Linear: BRK #0 | MOV x0,x0 (ORR x0,xzr,x0) | NOP
// Maximum confusion: 3 dead instructions, one is a breakpoint
// Assembled bytes: 04 00 00 14 | 00 00 20 D4 | E0 03 00 AA | 1F 20 03 D5
    .global cxs_ovpat_p3
    .global cxs_ovpat_p3_end
    .type   cxs_ovpat_p3, %function
cxs_ovpat_p3:
    b       .Lp3_resume         // B +4 insns (skips 3 dead insns = 12 bytes)
    brk     #0                  // dead BRK#0:   00 00 20 D4
    mov     x0, x0              // dead MOV x0,x0: E0 03 00 AA
    nop                         // dead NOP:       1F 20 03 D5
.Lp3_resume:
cxs_ovpat_p3_end:
    ret
    .size   cxs_ovpat_p3, cxs_ovpat_p3_end - cxs_ovpat_p3

// --- P4: B+2 / dead YIELD (HINT #1) --------------------------------------
// CPU:    B skips 1 instruction (4 bytes), lands on ret
// Linear: also decodes YIELD (0xD5033F1F, HINT #1 — scheduler yield)
// Looks like "cooperative multitasking hook" — odd place to yield
// Assembled bytes: 02 00 00 14 | 3F 20 03 D5
    .global cxs_ovpat_p4
    .global cxs_ovpat_p4_end
    .type   cxs_ovpat_p4, %function
cxs_ovpat_p4:
    b       .Lp4_resume         // B +2 insns
    yield                       // dead YIELD: 3F 20 03 D5  (HINT #1)
.Lp4_resume:
cxs_ovpat_p4_end:
    ret
    .size   cxs_ovpat_p4, cxs_ovpat_p4_end - cxs_ovpat_p4
    .section .note.GNU-stack,"",@progbits
