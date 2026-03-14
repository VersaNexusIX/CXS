; =============================================================================
; cxs_asm_x86_64.asm  - CXS x86-64 ASM helpers + T6 polyglot patterns  v1.2
;
; Assembled with: nasm -f elf64 cxs_asm_x86_64.asm
;
; T6 polyglot patterns — each cxs_ovpat_pN function contains a real
; short-jump (EB xx) that the CPU executes correctly, followed by dead
; bytes inside the gap.  A naive linear-sweep disassembler decodes the
; dead bytes as instructions, producing a different disassembly.
;
; The engine reads the bytes at runtime via:
;   len  = cxs_ovpat_pN_end - cxs_ovpat_pN   (linker symbols)
;   copy = memcpy(buf, cxs_ovpat_pN, len)
;
; x86-64 SysV ABI: args rdi/rsi/rdx, return rax, scratch r10-r15.
; =============================================================================

section .data
    cxs_magic  dq  0x43585300435853     ; "CXS\0CXS" ident marker

section .text

global cxs_xorshift64
global cxs_memcpy_volatile
global cxs_golden_fn
global cxs_add5_sub3
global cxs_nop_sled_8
global cxs_poly_marker

; T6 pattern symbols (start + end for runtime size calculation)
global cxs_ovpat_p0,     cxs_ovpat_p0_end
global cxs_ovpat_p1,     cxs_ovpat_p1_end
global cxs_ovpat_p2,     cxs_ovpat_p2_end
global cxs_ovpat_p3,     cxs_ovpat_p3_end
global cxs_ovpat_p4,     cxs_ovpat_p4_end

; =============================================================================
; uint64_t cxs_xorshift64(uint64_t seed)
; Arg: rdi=seed   Return: rax
; =============================================================================
cxs_xorshift64:
    mov     rax, rdi
    shl     rax, 13
    xor     rdi, rax            ; seed ^= seed << 13

    mov     rax, rdi
    shr     rax, 7
    xor     rdi, rax            ; seed ^= seed >> 7

    mov     rax, rdi
    shl     rax, 17
    xor     rdi, rax            ; seed ^= seed << 17

    mov     rax, rdi
    ret

; =============================================================================
; void cxs_memcpy_volatile(void *dst, const void *src, size_t n)
; Args: rdi=dst, rsi=src, rdx=n
; =============================================================================
cxs_memcpy_volatile:
    test    rdx, rdx
    jz      .done
.loop:
    mov     al, byte [rsi]
    mov     byte [rdi], al
    inc     rsi
    inc     rdi
    dec     rdx
    jnz     .loop
.done:
    ret

; =============================================================================
; int64_t cxs_golden_fn(int64_t x)
; f(x) = (x + 5 - 3) * 2     Arg: rdi   Return: rax
; =============================================================================
cxs_golden_fn:
    mov     rax, rdi
    add     rax, 5
    sub     rax, 3
    imul    rax, 2
    ret

; =============================================================================
; int64_t cxs_add5_sub3(int64_t x)
; =============================================================================
cxs_add5_sub3:
    mov     rax, rdi
    add     rax, 5
    sub     rax, 3
    ret

; =============================================================================
; void cxs_nop_sled_8(void)
; =============================================================================
cxs_nop_sled_8:
    times 8 nop
    ret

; =============================================================================
; void cxs_poly_marker(void)
; Encodes ASCII 'C','X','S' as actual x86 instruction bytes:
;   43 = REX.X prefix (in 64-bit context)
;   58 = POP rax
;   53 = PUSH rbx
; Sequence is harmless but signature is readable in a hex dump.
; =============================================================================
cxs_poly_marker:
    db  0x43, 0x58, 0x53        ; REX.X  POP rax  PUSH rbx  → spells "CXS"
    nop
    ret

; =============================================================================
; T6 Polyglot overlap patterns — assembled by NASM, read at runtime
;
; IMPORTANT: each pattern is a self-contained, CPU-executable byte sequence.
; The CPU follows the jump and executes normally.
; Only a LINEAR-SWEEP disassembler also decodes the dead bytes in the gap.
;
; Pattern structure:
;   [JMP SHORT +N]  [N dead bytes — valid x86 instructions]  [resume / ret]
;
; The engine copies bytes [cxs_ovpat_pN .. cxs_ovpat_pN_end) at runtime.
; =============================================================================

; --- P0: JMP SHORT +3 / shadow near-CALL ------------------------------------
; CPU sees: jmp resume_p0 ; (skips 3 dead bytes)
; Linear disasm also decodes: E8 FF FF = CALL -1   (incomplete near-call)
;                             00       = ADD [rax], al
; Bytes: EB 03 | E8 FF FF 00 | 90
cxs_ovpat_p0:
    jmp short .p0_resume        ; EB 03
    db  0xE8, 0xFF, 0xFF, 0x00  ; dead: looks like CALL -1  (never executed)
.p0_resume:
    nop                         ; 90 — padding to align pattern boundary
cxs_ovpat_p0_end:

; --- P1: JMP SHORT +4 / ENDBR64 ghost --------------------------------------
; CPU sees: jmp resume_p1 ; (skips 4 dead bytes)
; Linear disasm also decodes: F3 0F 1E FA = ENDBR64  (CET end-branch marker)
; Confuses CFI (Control Flow Integrity) checkers and call-graph tools.
; Bytes: EB 04 | F3 0F 1E FA | 90 90
cxs_ovpat_p1:
    jmp short .p1_resume        ; EB 04
    db  0xF3, 0x0F, 0x1E, 0xFA  ; dead: ENDBR64 (never executed)
.p1_resume:
    nop                         ; padding
    nop
cxs_ovpat_p1_end:

; --- P2: JMP SHORT +2 / JMP RAX shadow -------------------------------------
; CPU sees: jmp resume_p2 ; (skips 2 dead bytes)
; Linear disasm also decodes: FF E0 = JMP rax  (indirect jump — alarming!)
; Bytes: EB 02 | FF E0 | 90
cxs_ovpat_p2:
    jmp short .p2_resume        ; EB 02
    db  0xFF, 0xE0              ; dead: JMP rax (never executed)
.p2_resume:
    nop
cxs_ovpat_p2_end:

; --- P3: JMP SHORT +3 / multi-prefix NOP shadow ----------------------------
; CPU sees: jmp resume_p3 ; (skips 3 dead bytes)
; Linear disasm also decodes: 66 0F 1F = start of multi-byte NOP (incomplete)
; 66 = operand-size prefix, 0F 1F = NOP Ev — looks like benign padding.
; Bytes: EB 03 | 66 0F 1F | 90 90
cxs_ovpat_p3:
    jmp short .p3_resume        ; EB 03
    db  0x66, 0x0F, 0x1F        ; dead: multi-byte NOP prefix sequence
.p3_resume:
    nop
    nop
cxs_ovpat_p3_end:

; --- P4: JMP SHORT +5 / JMP32 DEADBEEF ghost --------------------------------
; CPU sees: jmp resume_p4 ; (skips 5 dead bytes)
; Linear disasm also decodes: E9 DE AD BE EF = JMP 0xEFBEADDE  (DEADBEEF!)
; Classic analyst bait — looks like deliberate obfuscation marker.
; Bytes: EB 05 | E9 DE AD BE EF | 90 90 90
cxs_ovpat_p4:
    jmp short .p4_resume        ; EB 05
    db  0xE9, 0xDE, 0xAD, 0xBE, 0xEF   ; dead: JMP DEADBEEF (never executed)
.p4_resume:
    nop
    nop
    nop
cxs_ovpat_p4_end:
