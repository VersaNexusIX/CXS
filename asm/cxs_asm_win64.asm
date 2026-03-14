; =============================================================================
; cxs_asm_win64.asm - CXS x86_64 ASM helpers for Windows (MASM / ML64)
;
; Assembled with: ml64 /c cxs_asm_win64.asm /Fo cxs_asm_win64.obj
;        or with: nasm -f win64 cxs_asm_win64.asm -o cxs_asm_win64.obj
;
; Windows x64 calling convention (Microsoft ABI):
;   Integer args : rcx, rdx, r8, r9  (NOT rdi/rsi like System V)
;   Return value : rax
;   Shadow space : 32 bytes RSP-allocated before CALL (handled by caller)
;   Volatile regs: rax, rcx, rdx, r8, r9, r10, r11
; =============================================================================

OPTION DOTNAME         ; allow dots in label names

.CODE

; =============================================================================
; uint64_t cxs_xorshift64(uint64_t seed)
;   Windows: seed in RCX, return in RAX
; =============================================================================
cxs_xorshift64 PROC
    mov     rax, rcx        ; rax = seed

    ; seed ^= seed << 13
    mov     rdx, rax
    shl     rdx, 13
    xor     rax, rdx

    ; seed ^= seed >> 7
    mov     rdx, rax
    shr     rdx, 7
    xor     rax, rdx

    ; seed ^= seed << 17
    mov     rdx, rax
    shl     rdx, 17
    xor     rax, rdx

    ret
cxs_xorshift64 ENDP

; =============================================================================
; void cxs_memcpy_volatile(void *dst, const void *src, size_t n)
;   Windows: dst=RCX, src=RDX, n=R8
; =============================================================================
cxs_memcpy_volatile PROC
    test    r8, r8
    jz      done_mv

loop_mv:
    mov     al, byte ptr [rdx]
    mov     byte ptr [rcx], al
    inc     rcx
    inc     rdx
    dec     r8
    jnz     loop_mv

done_mv:
    ret
cxs_memcpy_volatile ENDP

; =============================================================================
; int64_t cxs_golden_fn(int64_t x)    f(x) = (x + 5 - 3) * 2
;   Windows: x in RCX, return in RAX
; =============================================================================
cxs_golden_fn PROC
    mov     rax, rcx
    add     rax, 5          ; x + 5
    sub     rax, 3          ; x + 2
    imul    rax, 2          ; 2x + 4
    ret
cxs_golden_fn ENDP

; =============================================================================
; int64_t cxs_add5_sub3(int64_t x)
;   Windows: x in RCX, return in RAX
; =============================================================================
cxs_add5_sub3 PROC
    mov     rax, rcx
    add     rax, 5
    sub     rax, 3
    ret
cxs_add5_sub3 ENDP

; =============================================================================
; void cxs_nop_sled_8(void)
; =============================================================================
cxs_nop_sled_8 PROC
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    ret
cxs_nop_sled_8 ENDP

END
