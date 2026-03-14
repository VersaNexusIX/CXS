# Dokumentasi CXS — Code eXecution Scrambler v4.0

**CXS (Code eXecution Scrambler)** adalah sebuah engine *polymorphic self-modifying assembly* yang dirancang untuk mengaburkan (obfuscate) kode melalui 30 lapisan transformasi yang berbeda. Proyek ini mengubah representasi perantara (IR) menjadi kode assembly yang sulit dianalisis namun tetap mempertahankan fungsi semantik yang sama.

---

## Daftar Isi
1. [Fitur Utama](#fitur-utama)
2. [Instalasi](#instalasi)
3. [Panduan Penggunaan CLI](#panduan-penggunaan-cli)
4. [Arsitektur dan Pipeline](#arsitektur-dan-pipeline)
5. [Teknik Obfuskasi (T1 – T30)](#teknik-obfuskasi-t1--t30)
6. [Format File IR (.cxs)](#format-file-ir-cxs)
7. [ASM Emitter](#asm-emitter)
8. [Struktur Proyek](#struktur-proyek)

---

## Fitur Utama
- **30 Transformasi Berlapis**: Mencakup level instruksi, data, alur kontrol, dan anti-analisis.
- **Dukungan Multi-Arsitektur**: Mendukung x86-64 (Linux, macOS, Windows) dan ARM64 (Android, iOS, Apple Silicon).
- **Verifikasi Semantik**: Setiap proses obfuskasi diverifikasi secara otomatis menggunakan VM internal untuk memastikan output tetap bekerja seperti aslinya.
- **ASM Emitter**: Menghasilkan file `.S` yang siap di-assemble menggunakan GAS (GNU Assembler).

---

## Instalasi

CXS menggunakan `Makefile` untuk mempermudah proses kompilasi. Sistem akan mendeteksi platform dan arsitektur Anda secara otomatis.

```bash
# Kompilasi standar
make

# Kompilasi dengan simbol debug
make DEBUG=1

# Membersihkan hasil build
make clean

# Menjalankan tes stres (50 siklus verifikasi)
make stress
```

---

## Panduan Penggunaan CLI

Setelah dikompilasi, Anda dapat menjalankan binary `cxs` dengan berbagai opsi:

| Opsi | Deskripsi |
|------|-----------|
| `(tanpa argumen)` | Menjalankan demo interaktif menggunakan program sampel bawaan. |
| `-f <file.cxs>` | Mengaburkan file IR target. |
| `-f <file.cxs> -s N` | Mengaburkan file dan menjalankan `N` siklus tes stres untuk verifikasi. |
| `--stress N` | Menjalankan tes stres pada sampel bawaan sebanyak `N` kali. |
| `--emit-asm` | Menghasilkan file assembly (.S) dari hasil obfuskasi. |
| `-o <output.S>` | Menentukan nama file output assembly (digunakan bersama `--emit-asm`). |
| `--arm64` / `--x86-64` | Memaksa arsitektur output tertentu saat menggunakan emitter. |
| `--help` | Menampilkan pesan bantuan. |

---

## Arsitektur dan Pipeline

CXS bekerja melalui beberapa tahapan proses:
1. **Parsing**: Membaca kode sumber dari file `.cxs` atau sampel bawaan ke dalam bentuk **Intermediate Representation (IR)**.
2. **Snapshot**: Mengambil "Golden Snapshot" dari fungsi asli untuk referensi verifikasi.
3. **Transformation**: Menjalankan 30 tahap transformasi (T1 hingga T30) secara berurutan.
4. **Verification**: Menguji hasil obfuskasi terhadap 32 vektor input untuk memastikan output semantik tetap sama dengan aslinya.
5. **Emission**: (Opsional) Mengonversi IR yang telah diaburkan menjadi kode assembly asli (GAS AT&T untuk x86 atau AArch64 untuk ARM).

---

## Teknik Obfuskasi (T1 – T30)

Transformasi dibagi menjadi 4 lapisan utama:

### Layer 1: Tingkat Instruksi
Fokus pada penggantian dan modifikasi instruksi individu.
- **T1 (Arithmetic Substitution)**: Mengganti `ADD` dengan `SUB`, `INC` dengan `ADD 1`, dll.
- **T2 (Junk Injection)**: Menyisipkan instruksi sampah (`NOP`, `XOR`, `AND`) yang tidak mengubah status program.
- **T6 (Instruction Overlap)**: Menggunakan pola byte poliglot yang valid sebagai instruksi berbeda.
- **T11 (Instruction Substitution)**: Menukar opcode dengan padanan semantiknya.
- **T18 (Instruction Replication)**: Menggandakan instruksi yang bersifat netral.
- **T26 (Bitfield Noise)**: Menyisipkan urutan `BSWAP`/`ROR`/shift palsu pada register scratch.

### Layer 2: Data dan Operand
Mengaburkan cara data disimpan dan diakses.
- **T5 (Register Renaming)**: Rotasi register menggunakan algoritma Fisher-Yates.
- **T8 (Constant Encoding)**: Mengodekan nilai konstan menggunakan operasi XOR/ADD/ROL.
- **T12 (Data Flow Obfuscation)**: Memecah variabel ke dalam pasangan register.
- **T13 (Stack Frame Mangling)**: Menambah noise pada operasi stack (PUSH/POP palsu).
- **T16 (Data Encryption)**: Enkripsi rolling-XOR pada konstanta inline.
- **T20 (Alias Register Chains)**: Membuat rantai alias untuk register yang sedang digunakan.

### Layer 3: Alur Kontrol (Control Flow)
Mengubah struktur logika program agar sulit dipahami oleh decompiler.
- **T3 (Block Reordering)**: Mengacak urutan blok kode dan menyambungkannya kembali dengan `JMP`.
- **T4 (Opaque Predicates)**: Menambahkan percabangan yang selalu bernilai salah (menggunakan pola matematika).
- **T7 (Control Flow Flattening)**: Mengubah logika menjadi mesin status (state-machine) dalam satu loop besar.
- **T9 (Indirect Control Flow)**: Menggunakan tabel lompatan (jump table) dengan target terenkripsi.
- **T19 (CFG Virtualization Lite)**: Mengubah bagian kode menjadi bytecode mini-VM.

### Layer 4: Anti-Analisis
Teknik khusus untuk menghambat debugger dan alat analisis statis.
- **T14 (Polymorphic Decrypt Stub)**: Prolog pemecah kode yang bersifat *self-modifying*.
- **T15 (Anti-Analysis Markers)**: Jebakan menggunakan instruksi `CPUID` atau `RDTSC`.
- **T27 (Checksum Guards)**: Menambahkan pemeriksaan integritas kode secara real-time.
- **T30 (Multi-layer Key Schedule)**: Ekspansi kunci enkripsi melalui beberapa putaran operasi bitwise.

---

## Format File IR (.cxs)

File IR menggunakan sintaks mirip assembly yang disederhanakan. Setiap file terdiri dari blok-blok kode.

```asm
# Contoh program sederhana: f(x) = (x + 5) - 3
.block .entry
  label   .entry
  add     rax, 5      # Tambah 5 ke input (rax)
  sub     rax, 3      # Kurangi 3
  jmp     .done       # Lompat ke blok selesai
.end

.block .done
  label   .done
  ret                 # Kembali dengan hasil di rax
.end
```

**Register yang didukung:**
- **x86-64**: `rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`, `rsp`, `rbp`, `r8` s/d `r15`.
- **ARM64**: `x0` s/d `x15`, `sp`, `fp`.

---

## ASM Emitter

Gunakan flag `--emit-asm` untuk menghasilkan kode assembly yang dapat dikompilasi.

```bash
./cxs -f samples/double.cxs --emit-asm
```

Hasilnya akan berupa file `.S` (misalnya `obfuscated_x86_64.S`). Anda dapat meng-assemble file tersebut dengan:

```bash
# Untuk x86-64 Linux
cc -c -o out.o obfuscated_x86_64.S
```

Setiap instruksi yang dihasilkan akan diberi komentar label transformasinya, memudahkan pengembang untuk melacak teknik mana yang diterapkan pada baris tersebut.

---

## Struktur Proyek

- `src/`: Berisi kode sumber inti (C).
  - `main.c`: Logika antarmuka baris perintah (CLI).
  - `engine.c`: Virtual Machine, disassembler, dan verifikator.
  - `transform.c` & `transform2.c`: Implementasi teknik T1 hingga T30.
  - `emit.c`: Penghasil kode assembly GAS.
- `include/`: Header file (`cxs.h`, `cxs_emit.h`).
- `asm/`: Template assembly untuk berbagai arsitektur.
- `samples/`: Contoh file IR untuk pengujian.
- `Makefile`: Skrip build sistem.

---
*Dokumentasi ini dibuat untuk CXS v4.0 oleh VersaNexusIX.*

---

## Panduan Pengembangan: Menambahkan Transformasi Baru

Jika Anda ingin memperluas kemampuan CXS dengan teknik obfuskasi baru, ikuti langkah-langkah berikut:

1. **Definisikan Fungsi Transformasi**:
   Buat fungsi baru di `src/transform2.c` (atau file sumber baru) dengan tanda tangan berikut:
   ```c
   void cxs_transform_baru(cxs_engine_t *e) {
       // Logika transformasi Anda di sini
   }
   ```

2. **Iterasi Instruksi**:
   Gunakan loop untuk memproses instruksi yang ada:
   ```c
   for (int i = 0; i < e->num_insns; i++) {
       insn_t *ins = &e->insns[i];
       // Hindari memodifikasi instruksi sampah atau label kecuali diperlukan
       if (ins->is_junk || ins->type == INSN_LABEL) continue;
       
       // Contoh: Ubah sesuatu
   }
   ```

3. **Gunakan Helper Internal**:
   Gunakan fungsi seperti `insn_insert()` untuk menyisipkan instruksi di tengah aliran, atau `op_reg()`/`op_imm()` untuk membuat operand baru.

4. **Daftarkan di Pipeline**:
   Tambahkan panggilan fungsi Anda di dalam `cxs_run_pipeline()` yang terletak di `src/transform.c`.

5. **Update Statistik**:
   Tambahkan kolom baru di struktur `cxs_stats_t` dalam `cxs.h` untuk melacak jumlah transformasi yang berhasil diterapkan oleh teknik baru Anda.

---

## Lisensi dan Kontribusi

Proyek ini dikembangkan oleh **VersaNexusIX**. Kontribusi dalam bentuk *bug report*, saran teknik obfuskasi baru, atau *pull request* sangat dihargai untuk pengembangan versi berikutnya.
