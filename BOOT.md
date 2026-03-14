# BOOT.md — Boot Flow MyOS ARM64

## Gambaran Umum

MyOS boot dari satu binary (`kernel8.img` atau `kernel.elf`) yang di-load langsung oleh QEMU. Tidak ada bootloader perantara — QEMU meletakkan binary di memory dan melompat ke `_start`.

---

## Diagram Lengkap

```
QEMU -M virt -cpu cortex-a53 -kernel kernel8.img
         │
         ▼ CPU reset @ EL2
┌─────────────────────────────────────────────────────┐
│  boot/start.S — _start                              │
│                                                     │
│  mrs x0, CurrentEL                                  │
│  lsr x0, x0, #2                                     │
│                                                     │
│  ┌── jika EL2 ──────────────────────────────────┐  │
│  │  hcr_el2  = (1<<31)    ; RW=1 → EL1 AArch64 │  │
│  │  spsr_el2 = 0x3c5      ; EL1h, IRQ masked    │  │
│  │  elr_el2  = el1_setup                        │  │
│  │  eret ────────────────────────────────────┐  │  │
│  └───────────────────────────────────────────┼──┘  │
│                                              │      │
│  ┌── el1_setup ◄─────────────────────────────┘     │
│  │  bss_loop:                                       │
│  │    str xzr, [x0], #8   ; zero .bss section      │
│  │    subs x1, x1, #8                               │
│  │    b.gt bss_loop                                 │
│  │                                                  │
│  │  adr sp, __stack_top   ; SP = top of 64KB stack  │
│  │  bl  kernel_main                                 │
│  └──────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  kernel/main.c — kernel_main()                      │
│                                                     │
│  [1]  uart_init()                                   │
│         PL011 @ 0x09000000, 115200 baud 8N1         │
│         → UART siap, output bisa dimulai            │
│                                                     │
│  [2]  msr vbar_el1, &vectors                        │
│         dsb sy / isb                                │
│         → exception table aktif                     │
│                                                     │
│  [3]  mm_init()                                     │
│         heap @ 0x40800000, ukuran 16 MB             │
│         → kmalloc / kfree siap                      │
│                                                     │
│  [4]  error_init()                                  │
│         → subsistem error siap                      │
│                                                     │
│  [5]  proc_init()                                   │
│         → tabel proses 32 slot di-clear             │
│                                                     │
│  [6]  fs_init()                                     │
│         → buat direktori: /, /bin, /etc, /tmp       │
│              /home, /usr, /var, /usr/bin, dll       │
│         → seed file: /etc/hostname, /etc/version    │
│              /etc/motd, /etc/shells                 │
│                                                     │
│  [7]  net_init()                                    │
│         VirtIO-NET MMIO @ 0x0A000000                │
│         IP default: 10.0.2.15                       │
│         GW default: 10.0.2.2                        │
│                                                     │
│  [8]  pkg_init()                                    │
│         buat dir /var/pkg, /var/pkg/cache, dll      │
│         load /var/pkg/db jika ada                   │
│                                                     │
│  [9]  shell_run()                                   │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────────────────────┐
│  kernel/shell.c — shell_run()                       │
│                                                     │
│  clear screen                                       │
│  print banner neofetch (logo + sysinfo)             │
│  baca /etc/motd jika ada                            │
│                                                     │
│  loop selamanya:                                    │
│    show_prompt()   →  root@myos:/# _                │
│    readline()      →  baca input per karakter       │
│    parse_args()    →  tokenize spasi/tab            │
│    dispatch:                                        │
│      ├─ built-in cmd   → jalankan langsung          │
│      ├─ /usr/bin/<n>   → exec_load() + exec_run()  │
│      └─ tidak dikenal  → cmd_not_found()            │
│           ├─ cek tabel typo (50+ entri)             │
│           ├─ levenshtein distance ≤ 2               │
│           └─ saran mos -s / mos -i                  │
└─────────────────────────────────────────────────────┘
```

---

## EL2 → EL1: Kenapa Perlu?

QEMU `virt` machine menyalakan CPU di **EL2** (Exception Level 2 = hypervisor). Kernel sistem operasi normal berjalan di **EL1**. Oleh karena itu `start.S` perlu "turun" dulu ke EL1 sebelum menjalankan kernel.

| Level | Nama | Dipakai untuk |
|-------|------|--------------|
| EL3 | Secure Monitor | Firmware (TrustZone) |
| EL2 | Hypervisor | Virtualisasi (QEMU start di sini) |
| EL1 | Kernel | **MyOS berjalan di sini** |
| EL0 | Userspace | Program biasa |

Kode EL2 setup:
```asm
; Aktifkan AArch64 di EL1 (bit RW = 1)
mov  x0, #(1 << 31)
msr  hcr_el2, x0

; Target state: EL1h (SP_EL1), semua IRQ masked
mov  x0, #0x3c5
msr  spsr_el2, x0

; Return address setelah eret = el1_setup
adr  x0, el1_setup
msr  elr_el2, x0

; Eksekusi kembali ke EL1
eret
```

---

## BSS Clear

Section `.bss` berisi semua variabel global yang tidak diinisialisasi. Standar C mengharuskan nilainya nol saat program start. Di bare-metal tidak ada OS yang melakukan ini otomatis, sehingga `start.S` harus melakukannya sendiri:

```asm
el1_setup:
    adr  x0, __bss_start
    adr  x1, __bss_end
    sub  x1, x1, x0        ; hitung panjang BSS

bss_loop:
    str  xzr, [x0], #8     ; tulis 8 byte nol, maju 8
    subs x1, x1, #8
    b.gt bss_loop
```

---

## Stack

Stack dialokasikan sebagai array statis di `.data`:

```asm
.section .data
.align 4
__stack_bottom:
    .space 0x10000          ; 64 KB
__stack_top:
```

SP diset ke `__stack_top` karena ARM64 menggunakan **full-descending stack** — stack tumbuh ke alamat yang lebih kecil.

---

## Memory Map Lengkap

```
Alamat Fisik       Ukuran    Konten
─────────────────  ────────  ──────────────────────────────
0x09000000         MMIO      PL011 UART
0x0A000000         MMIO      VirtIO-NET MMIO registers
0x0A003E00         MMIO      VirtIO-NET config space

0x40000000         RAM       Awal RAM QEMU virt
0x40080000         .text     Kernel load address (_start)
0x40081000         .text     Kernel code
0x40090000         .rodata   String literals, konstanta
0x400A0000         .data     Initialized globals, stack (64 KB)
0x400B0000         .bss      Zero-initialized globals

0x40800000         16 MB     Heap (kmalloc pool)
0x41800000                   Batas akhir heap
```

> Kernel di-load di `0x40080000` karena QEMU `virt` menyediakan RAM dari `0x40000000`, dan 512 KB pertama sengaja dikosongkan sebagai buffer aman.

---

## Linker Script

```ld
/* kernel.ld */
ENTRY(_start)

SECTIONS {
    . = 0x40080000;

    .text.boot : { *(.text.boot) }   /* _start di paling depan */
    .text      : { *(.text*)     }
    .rodata    : { *(.rodata*)   }
    .data      : { *(.data*)     }

    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    __bss_end = .;
}
```

---

## Exception Vector Table

Setelah `uart_init()`, vector table dipasang ke `VBAR_EL1`:

```c
/* kernel/main.c */
asm volatile(
    "msr vbar_el1, %0\n"
    "dsb sy\n"
    "isb\n"
    :: "r"(vectors) : "memory"
);
```

`vectors.S` menggunakan macro `save_regs` yang menyimpan semua register sebelum memanggil handler C:

```asm
.macro save_regs type
    sub  sp, sp, #(35 * 8)      ; alokasi frame di stack
    stp  x0,  x1,  [sp, #0]     ; simpan x0–x1
    ; ... semua register x0–x30
    mrs  x0, elr_el1             ; PC saat exception
    str  x0, [sp, #(32*8)]
    mrs  x0, esr_el1             ; exception syndrome
    str  x0, [sp, #(34*8)]
    mrs  x1, far_el1             ; fault address
    str  x1, [sp, #(35*8-8)]

    mov  x0, sp                  ; arg1: pointer ke regs
    mov  x1, #\type              ; arg2: tipe exception
    bl   exception_handler       ; panggil C handler
.endm
```

`exception_handler()` menampilkan informasi fault dan melakukan countdown reboot 5 detik. Tidak ada `b .` (hang selamanya).
