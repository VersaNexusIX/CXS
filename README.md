# MyOS — ARM64 Monolithic Kernel

```
  █▀▄▀█ █▄█ █▀█ █▀
  █░▀░█ ░█░ █▄█ ▄█
```

**MyOS** adalah kernel monolitik minimalis untuk arsitektur **ARM64 (AArch64)**, ditulis dari nol dalam C dan Assembly. Berjalan langsung di QEMU `virt` machine tanpa bootloader tambahan.

---

## Fitur

| Komponen | Detail |
|----------|--------|
| Arsitektur | AArch64 — EL2 → EL1 drop |
| UART | PL011 driver @ `0x09000000` |
| Memory | Heap allocator — kmalloc / kfree |
| Filesystem | ramfs in-memory, 64 node, 4 KB per file |
| Network | VirtIO-NET + stack TCP/IP |
| HTTP | HTTP/1.1 client (GET + download ke file) |
| Archive | Extractor TAR ustar + ZIP stored/deflate |
| Exec | ELF64 loader untuk binary ARM64 |
| Shell | CLI interaktif, error fuzzy matching |
| Package Manager | `mos` — install dari GitHub, URL, atau registry |
| Error Handler | Exception handler + auto reboot, bukan hang selamanya |

---

## Struktur Direktori

```
myos/
├── boot/
│   └── start.S           # Entry point ARM64
├── kernel/
│   ├── main.c            # kernel_main() — init semua subsistem
│   ├── shell.c           # Shell interaktif
│   ├── vectors.S         # ARM64 exception vector table
│   └── error.c           # Error handler, panic, fuzzy suggest
├── drivers/
│   └── uart.c            # PL011 UART driver
├── mm/
│   └── mm.c              # Heap allocator (first-fit + coalescing)
├── lib/
│   ├── string.c          # String utilities (tanpa libc)
│   ├── archive.c         # TAR + ZIP extractor + CRC32
│   ├── exec.c            # ELF64 ARM64 loader
│   └── pkg.c             # Package manager (mos)
├── fs/
│   └── fs.c              # In-memory ramfs
├── proc/
│   └── proc.c            # Process table
├── net/
│   ├── net.c             # VirtIO-NET driver + TCP/IP stack
│   └── http.c            # HTTP/1.1 client
├── include/              # Semua header (.h)
├── kernel.ld             # Linker script — load @ 0x40080000
└── Makefile
```

---

## Quick Start

```bash
# Install toolchain + QEMU
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-arm

# Build kernel
make

# Jalankan di QEMU
make run

# Keluar QEMU: Ctrl+A lalu X
```

Setelah boot, akan tampil banner neofetch-style dan prompt:

```
  █▀▄▀█ █▄█ █▀█ █▀   root@myos
  █░▀░█ ░█░ █▄█ ▄█   -------------------
                  OS         MyOS 1.0 ARM64
                  Kernel     1.0.0 monolithic
                  ...

root@myos:/# _
```

---

## Dokumentasi

| File | Isi |
|------|-----|
| [BOOT.md](BOOT.md) | Boot flow lengkap, EL2→EL1, memory map, vector table |
| [SHELL.md](SHELL.md) | Semua perintah shell beserta contoh penggunaan |
| [PACKAGES.md](PACKAGES.md) | Package manager `mos` — GitHub install, URL, format mos.pkg |
| [NETWORK.md](NETWORK.md) | TCP/IP stack, VirtIO-NET, HTTP client, API reference |
| [BUILD.md](BUILD.md) | Build system, Makefile targets, QEMU flags, debugging |
| [INTERNALS.md](INTERNALS.md) | Kernel internals — MM, FS, ELF loader, error system |
