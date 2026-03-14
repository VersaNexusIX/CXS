# INTERNALS.md — Kernel Internals MyOS

## Desain: Monolitik

Semua subsistem berjalan dalam satu address space di **EL1** (kernel privilege). Tidak ada pemisahan userspace/kernelspace, tidak ada syscall ABI, tidak ada virtual memory. Fungsi dipanggil langsung antar subsistem sebagai C function call biasa.

```
┌──────────────────────────────────────────────────┐
│               EL1 — Kernel Space                 │
│                                                  │
│  shell → pkg → http → tcp → virtio → HW         │
│    │                                              │
│    ├→ fs  (ramfs, baca/tulis file)                │
│    ├→ mm  (kmalloc, kfree)                        │
│    └→ proc (ps, kill)                             │
│                                                  │
│  Exception → vectors.S → error.c → reboot        │
└──────────────────────────────────────────────────┘
```

---

## Memory Manager (`mm/mm.c`)

### Struktur Heap

Heap dimulai di `0x40800000`, ukuran 16 MB. Seluruh heap dikelola sebagai linked list blok:

```
Heap: 0x40800000 ──────────────────────────── 0x41800000

┌──────────┬──────────┬──────────┬──────────┬─────┐
│ hdr │data│ hdr │data│ hdr │data│ hdr │data│ ... │
└──────────┴──────────┴──────────┴──────────┴─────┘
```

Setiap blok memiliki header:
```c
typedef struct block_hdr {
    size_t            size;   // ukuran data (tidak termasuk header)
    int               free;   // 1 = bebas, 0 = dipakai
    struct block_hdr *next;   // pointer ke blok berikutnya
} block_hdr_t;
```

### `kmalloc(size)`

Algoritma **first-fit**:
1. Traverse linked list dari awal
2. Temukan blok pertama yang `free == 1` dan `size >= request`
3. Jika blok jauh lebih besar, **split** — sisanya jadi blok baru yang bebas
4. Tandai `free = 0`, return pointer ke data (setelah header)

### `kfree(ptr)`

1. Dapatkan header dari `ptr - sizeof(block_hdr_t)`
2. Tandai `free = 1`
3. **Coalesce** — gabungkan blok bebas yang bersebelahan untuk mencegah fragmentasi

### Batasan

| Parameter | Nilai |
|-----------|-------|
| Heap start | `0x40800000` |
| Heap size | 16 MB (`0x1000000`) |
| Overhead per alokasi | `sizeof(block_hdr_t)` = 24 bytes |
| Thread safety | Tidak ada (single-core, no preemption) |

---

## Filesystem (`fs/fs.c`)

### Representasi

ramfs disimpan sebagai array statis 64 node:

```c
static fs_node_t nodes[FS_MAX_FILES];  // 64 node

typedef struct fs_node {
    char      name[64];     // path absolut lengkap, e.g. "/etc/hostname"
    fs_type_t type;         // FS_FILE atau FS_DIR
    size_t    size;
    char      data[4096];   // konten file (max 4096 byte)
    int       used;         // 1 = slot dipakai
    int       parent;       // index node parent (direktori)
    int       id;           // index dalam array
} fs_node_t;
```

Tidak ada inode terpisah, tidak ada block allocation — setiap node langsung menyimpan datanya.

### Batasan

| Parameter | Nilai |
|-----------|-------|
| `FS_MAX_FILES` | 64 node |
| `FS_NAME_LEN` | 64 karakter per path |
| `FS_MAX_SIZE` | 4096 byte per file |
| Total kapasitas | ~256 KB |

### Operasi Utama

```c
int  fs_create(const char *path, const char *data, size_t len);
// Cari slot kosong, isi name+data+size, tandai used=1

int  fs_read(const char *path, char *buf, size_t len);
// Linear search nama == path, copy data ke buf

int  fs_write(const char *path, const char *data, size_t len);
// Cari node, update data+size

int  fs_delete(const char *path);
// Cari node, set used=0

void fs_list(const char *path);
// Iterasi semua node, filter yang parent == node[path]
```

### Direktori Default saat Boot

```
fs_init() membuat:
  /          (root)
  /bin
  /etc
  /home
  /tmp
  /usr
  /usr/bin
  /usr/lib
  /usr/include
  /usr/share
  /usr/local
  /usr/local/bin
  /var
  /var/pkg
  /var/pkg/cache
  /var/pkg/src
  /var/pkg/build
  /var/pkg/log
  /lib
  /root

File awal:
  /etc/hostname   → "myos"
  /etc/version    → "MyOS 1.0 ARM64"
  /etc/motd       → (pesan selamat datang)
  /etc/shells     → "/usr/bin/mysh"
```

---

## String Library (`lib/string.c`)

Tidak ada libc — semua fungsi string diimplementasi sendiri dengan prefix `k`:

```c
size_t kstrlen(const char *s);
int    kstrcmp(const char *a, const char *b);
int    kstrncmp(const char *a, const char *b, size_t n);
char  *kstrcpy(char *dst, const char *src);
char  *kstrncpy(char *dst, const char *src, size_t n);
char  *kstrcat(char *dst, const char *src);
char  *kstrtok(char *str, const char *delim);  // stateful, tidak reentrant
int    katoi(const char *s);                    // string → int
void   kitoa(long n, char *buf, int base);      // int → string (base 10/16)

void  *kmemset(void *ptr, int val, size_t len);
void  *kmemcpy(void *dst, const void *src, size_t len);
int    kmemcmp(const void *a, const void *b, size_t len);
```

---

## ELF Loader (`lib/exec.c`)

### Format ELF64 ARM64

```
ELF Header (64 bytes):
  magic    = 0x7F 'E' 'L' 'F'
  class    = 2 (64-bit)
  data     = 1 (little-endian)
  machine  = 0xB7 (AArch64)
  type     = 2 (ET_EXEC) atau 3 (ET_DYN)
  e_entry  = virtual address entry point
  e_phoff  = offset ke program header table

Program Headers (PT_LOAD segments):
  p_type   = 1 (PT_LOAD)
  p_offset = offset dalam file
  p_vaddr  = virtual address tujuan
  p_filesz = ukuran dalam file
  p_memsz  = ukuran dalam memori (≥ filesz, bss padding)
```

### Proses Load

```c
int exec_load(const char *path, exec_info_t *info) {
    // 1. Baca file dari ramfs ke buffer sementara
    fs_read(path, raw_buf, sizeof(raw_buf));

    // 2. Validasi
    //    - magic == 0x464C457F
    //    - ei_class == 2 (64-bit)
    //    - e_machine == 183 (0xB7 = AArch64)

    // 3. Scan program headers untuk PT_LOAD
    //    Hitung min_vaddr dan max_vaddr

    // 4. Alokasi buffer load (static 2MB)
    static char load_mem[0x200000];

    // 5. Copy tiap PT_LOAD segment ke load_mem
    //    dest = load_mem + (phdr.p_vaddr - min_vaddr)
    //    src  = raw_buf  + phdr.p_offset

    // 6. Hitung entry point
    //    entry = (uint64_t)load_mem + (ehdr.e_entry - min_vaddr)

    info->entry = entry;
    info->valid = 1;
}
```

### Eksekusi

```c
int exec_run(exec_info_t *info, int argc, char **argv) {
    typedef int (*entry_fn)(int, char**);
    entry_fn fn = (entry_fn)info->entry;
    return fn(argc, argv);
}
```

Binary langsung dipanggil sebagai C function. Return value-nya dikembalikan ke shell.

### Batasan

- Binary harus **statically linked** (tidak ada dynamic linker)
- Maksimum ukuran binary: 2 MB (ukuran `load_mem`)
- Binary bisa memanggil fungsi kernel yang diekspos via header (jika di-link saat build MyOS)

---

## Error System (`kernel/error.c`)

### Tiga Level Error

```
Level 1: Shell error — typo, command not found
  cmd_not_found() → tampilkan "Did you mean X?"

Level 2: Kernel error — subsistem gagal
  kernel_error()  → tampilkan pesan, lanjut
  PANIC(msg)      → tampilkan pesan, reboot 5 detik

Level 3: Hardware exception — CPU fault
  exception_handler() → dump register, reboot 5 detik
```

### Exception Handler

Dipanggil dari `vectors.S` dengan dua argumen:
- `cpu_regs_t *regs` — pointer ke frame register di stack
- `int type` — index entry vector table (0–15)

```c
typedef struct {
    uint64_t x0, x1, ..., x30;   // general purpose registers
    uint64_t sp;                   // stack pointer
    uint64_t pc;                   // program counter (ELR_EL1)
    uint64_t spsr;                 // saved program status
    uint64_t esr;                  // exception syndrome register
    uint64_t far;                  // fault address register
} cpu_regs_t;
```

ESR_EL1 decode:
```
ESR[31:26] = EC  (exception class, 6 bit)
ESR[25]    = IL  (instruction length)
ESR[24:0]  = ISS (instruction specific syndrome)
```

| EC | Artinya |
|----|---------|
| `0x24` | Data Abort EL0 — null pointer / bad address dari "userspace" |
| `0x25` | Data Abort EL1 — null pointer / bad address di kernel |
| `0x20` | Instruction Abort EL0 — jump ke alamat tidak valid |
| `0x21` | Instruction Abort EL1 — jump ke alamat tidak valid di kernel |
| `0x2F` | SError — hardware/bus fault async |

### Reboot Mechanism

```c
static void reboot_now(void) {
    asm volatile("dsb sy\nisb\n" ::: "memory");
    asm volatile("hvc #0");   // QEMU: HVC #0 = reset
    while(1) asm volatile("wfe");
}
```

`hvc #0` memicu PSCI (Power State Coordination Interface) reset di QEMU. Di hardware nyata perlu implementasi PSCI yang proper atau menulis ke watchdog register.

### Levenshtein Distance

Dipakai untuk fuzzy matching "Did you mean?" di shell:

```c
int levenshtein(const char *a, const char *b) {
    // Implementasi DP O(n*m)
    // dp[i][j] = edit distance antara a[0..i] dan b[0..j]
    // Operasi: insert, delete, substitute (masing-masing cost 1)
    // Ukuran maksimum: 32 karakter (array statis 33x33)
}
```

Threshold yang dipakai: jika jarak ≤ 2 dan jarak < panjang input, tampilkan saran.

---

## Process Table (`proc/proc.c`)

```c
#define MAX_PROCS 32

typedef enum {
    PROC_FREE,
    PROC_READY,
    PROC_RUNNING,
    PROC_SLEEPING,
    PROC_ZOMBIE
} proc_state_t;

typedef struct proc {
    int          pid;
    char         name[32];
    proc_state_t state;
    uint64_t     regs[31];
    uint64_t     sp;
    uint64_t     pc;
    uint64_t     stack[1024];   // 8 KB stack per proses
} proc_t;
```

Tidak ada scheduler aktif — MyOS adalah single-threaded. `proc_create()` mengalokasikan slot dan mengisi metadata, tapi tidak benar-benar menjadwalkan eksekusi terpisah. Ini adalah placeholder untuk scheduler di versi mendatang.

---

## Konstanta Penting

```c
/* mm.h */
#define HEAP_START  0x40800000
#define HEAP_SIZE   0x1000000   // 16 MB

/* fs.h */
#define FS_MAX_FILES  64
#define FS_NAME_LEN   64
#define FS_MAX_SIZE   4096

/* net.h */
#define MAX_TCP_CONNS  8
#define NET_MTU        1514
#define VIRTIO_MMIO_BASE  0x0A000000

/* http.h */
#define HTTP_RX_BUF_SIZE  65536   // 64 KB

/* pkg.h */
#define PKG_MAX       128
#define PKG_MAX_DEPS   16
#define PKG_MAX_FILES  64

/* proc.h */
#define MAX_PROCS  32

/* error.h */
#define REBOOT_DELAY_TICKS  3000000
#define PANIC(msg)  kernel_error_reboot(msg, 5)
```
