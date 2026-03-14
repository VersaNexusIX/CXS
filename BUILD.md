# BUILD.md — Build System MyOS

## Requirements

| Tool | Versi minimum | Fungsi |
|------|--------------|--------|
| `aarch64-linux-gnu-gcc` | 11+ | Cross-compiler ARM64 |
| `aarch64-linux-gnu-ld` | binutils 2.38+ | Linker |
| `aarch64-linux-gnu-objcopy` | binutils 2.38+ | Buat binary image |
| `qemu-system-aarch64` | 6.0+ | Emulator |

### Install di Ubuntu/Debian

```bash
sudo apt update
sudo apt install \
    gcc-aarch64-linux-gnu \
    binutils-aarch64-linux-gnu \
    qemu-system-arm
```

### Install di Arch Linux

```bash
sudo pacman -S aarch64-linux-gnu-gcc aarch64-linux-gnu-binutils qemu-arch-extra
```

### Install di macOS (Homebrew)

```bash
brew install aarch64-elf-gcc aarch64-elf-binutils qemu
# Ganti prefix di Makefile: CROSS = aarch64-elf-
```

---

## Makefile Targets

```bash
make              # build kernel8.img (default)
make run          # build + jalankan di QEMU (binary image)
make run-elf      # build + jalankan di QEMU (ELF file)
make run-gdb      # jalankan QEMU dengan GDB server (tunggu di port 1234)
make clean        # hapus semua file hasil build
```

---

## Proses Build

```
Source files (.c, .S)
         │
         │  aarch64-linux-gnu-gcc -ffreestanding -nostdlib
         │  -nostdinc -O2 -mgeneral-regs-only
         ▼
Object files (.o)
         │
         │  aarch64-linux-gnu-ld -T kernel.ld
         ▼
kernel.elf  (ELF64, load @ 0x40080000)
         │
         │  aarch64-linux-gnu-objcopy -O binary
         ▼
kernel8.img  (raw binary, siap di-load QEMU)
```

---

## Compiler Flags

```makefile
CFLAGS = -ffreestanding    # tidak pakai runtime C standar
         -nostdlib         # tidak link libc atau libgcc
         -nostdinc         # tidak pakai system include
         -O2               # optimasi level 2
         -Wall -Wextra     # semua warning aktif
         -fno-builtin      # tidak pakai builtin GCC (memset, memcpy, dll)
         -fno-stack-protector  # tidak ada stack canary (tidak ada libc)
         -mgeneral-regs-only   # tidak pakai SIMD/FP register
         -I./include       # cari header di ./include/
```

**Kenapa `-mgeneral-regs-only`?**
Register SIMD/FP (v0–v31) perlu disave/restore saat exception. Karena exception handler MyOS belum menghandle ini, flag ini memastikan compiler tidak pernah menggunakan register tersebut di kernel code.

---

## QEMU Flags

### `make run` (kernel8.img)

```bash
qemu-system-aarch64 \
    -M virt \                          # machine: virt (generic ARM64)
    -cpu cortex-a53 \                  # emulasi Cortex-A53
    -m 128M \                          # 128 MB RAM
    -nographic \                       # tidak ada GUI, semua via serial
    -kernel kernel8.img \              # load binary image
    -netdev user,id=net0 \             # NAT network
    -device virtio-net-device,netdev=net0 \  # VirtIO-NET card
    -serial mon:stdio                  # serial ke stdout, monitor juga aktif
```

### `make run-elf` (kernel.elf)

Sama dengan atas tapi menggunakan `-kernel kernel.elf`. QEMU bisa load ELF langsung dan tahu entry point-nya tanpa perlu binary image murni.

### `make run-gdb` (debug mode)

```bash
qemu-system-aarch64 \
    ... \
    -s \   # buka GDB server di localhost:1234
    -S     # pause CPU saat start, tunggu GDB connect
```

Lalu di terminal lain:
```bash
aarch64-linux-gnu-gdb kernel.elf
(gdb) target remote localhost:1234
(gdb) break kernel_main
(gdb) continue
```

---

## Keluar dari QEMU

| Cara | Keterangan |
|------|-----------|
| `Ctrl+A` lalu `X` | Keluar QEMU langsung |
| `Ctrl+A` lalu `C` | Masuk QEMU monitor |
| `halt` di shell MyOS | Halt sistem (CPU spin) |
| `reboot` di shell MyOS | Reset via `hvc #0` |

Di QEMU monitor:
```
(qemu) quit       # keluar
(qemu) info mem   # lihat memory
(qemu) info registers  # lihat register CPU
```

---

## Struktur Output Build

```
myos/
├── boot/start.o
├── kernel/
│   ├── vectors.o
│   ├── main.o
│   ├── shell.o
│   └── error.o
├── drivers/uart.o
├── mm/mm.o
├── lib/
│   ├── string.o
│   ├── archive.o
│   ├── exec.o
│   └── pkg.o
├── fs/fs.o
├── proc/proc.o
├── net/
│   ├── net.o
│   └── http.o
├── kernel.elf       ← ELF dengan debug symbols
└── kernel8.img      ← raw binary untuk QEMU
```

---

## Debugging

### Lihat simbol

```bash
aarch64-linux-gnu-nm kernel.elf | grep kernel_main
aarch64-linux-gnu-objdump -d kernel.elf | less
```

### Lihat sections

```bash
aarch64-linux-gnu-readelf -S kernel.elf
```

### Ukuran per section

```bash
aarch64-linux-gnu-size kernel.elf
```

Output contoh:
```
   text    data     bss     dec     hex filename
  84320    1024    2048   87392   155a0 kernel.elf
```

### Disassemble fungsi tertentu

```bash
aarch64-linux-gnu-objdump -d kernel.elf | grep -A 50 "<kernel_main>:"
```

### GDB + QEMU (step-by-step)

```bash
# Terminal 1: jalankan QEMU
make run-gdb

# Terminal 2: connect GDB
aarch64-linux-gnu-gdb kernel.elf
(gdb) target remote :1234
(gdb) set architecture aarch64
(gdb) break kernel_main
(gdb) break exception_handler
(gdb) continue

# Saat breakpoint tercapai:
(gdb) info registers
(gdb) x/10i $pc          # disassemble 10 instruksi dari PC
(gdb) x/16xg $sp         # lihat 16 qword dari stack
(gdb) backtrace
(gdb) next
(gdb) step
```

---

## Cross-Compile Binary untuk MyOS

Untuk membuat binary ELF64 ARM64 yang bisa dijalankan via `exec` di MyOS:

```c
/* hello.c */
void uart_puts(const char *s);  /* deklarasi fungsi kernel */

void _start(void) {
    uart_puts("Hello from ELF!\n");
    /* tidak ada exit(), kernel akan return ke shell */
}
```

```bash
aarch64-linux-gnu-gcc \
    -ffreestanding -nostdlib -nostdinc \
    -O2 -static \
    -e _start \
    -o hello.elf hello.c

# Upload ke MyOS via metode apapun (hardcode di build, atau via network)
# Lalu di MyOS:
# exec /usr/bin/hello
```

> Binary yang di-load via `exec` atau `mos` harus ELF64 ARM64 tanpa dynamic linking (static binary atau binary yang hanya depend ke fungsi kernel yang sudah ada).

---

## Tambah Subsistem Baru

1. Buat file `newsub/newsub.c` dan `include/newsub.h`
2. Tambah ke `OBJS` di Makefile:
   ```makefile
   OBJS = ... \
          newsub/newsub.o
   ```
3. Include header di `kernel/main.c` dan panggil init function
4. Build: `make`

Tidak perlu konfigurasi tambahan — linker akan otomatis resolve semua simbol.
