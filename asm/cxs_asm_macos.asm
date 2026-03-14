; =============================================================================
; cxs_asm_macos.asm - CXS x86_64 ASM helpers for macOS / iOS (Mach-O)
;
; Assembled with: nasm -f macho64 cxs_asm_macos.asm -o cxs_asm_macos.o
;
; Note: On Apple Silicon (ARM64) the C compiler handles everything;
;       this file targets x86_64 macOS (Rosetta or native).
;       macOS requires an underscore prefix on exported C-visible symbols.
; =============================================================================

section .data
    align   8
    _cxs_magic  dq  0x43585300435853

section .text

; macOS requires leading underscore for C-linkage symbols
global _cxs_xorshift64
global _cxs_memcpy_volatile
global _cxs_golden_fn
global _cxs_nop_sled_8
global _cxs_add5_sub3

; =============================================================================
; uint64_t cxs_xorshift64(uint64_t seed)
; =============================================================================
_cxs_xorshift64:
    mov     rax, rdi
    shl     rax, 13
    xor     rdi, rax

    mov     rax, rdi
    shr     rax, 7
    xor     rdi, rax

    mov     rax, rdi
    shl     rax, 17
    xor     rdi, rax

    mov     rax, rdi
    ret

; =============================================================================
; void cxs_memcpy_volatile(void *dst, const void *src, size_t n)
; =============================================================================
_cxs_memcpy_volatile:
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
; int64_t cxs_golden_fn(int64_t x)   f(x) = (x + 5 - 3) * 2
; =============================================================================
_cxs_golden_fn:
    mov     rax, rdi
    add     rax, 5          ; x + 5
    sub     rax, 3          ; x + 2
    imul    rax, 2          ; 2x + 4
    ret

; =============================================================================
; int64_t cxs_add5_sub3(int64_t x)
; =============================================================================
_cxs_add5_sub3:
    mov     rax, rdi
    add     rax, 5
    sub     rax, 3
    ret

; =============================================================================
; void cxs_nop_sled_8(void)
; =============================================================================
_cxs_nop_sled_8:
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    ret
