# API CXS — Code eXecution Scrambler v4.0

API (Application Programming Interface) CXS menyediakan fungsi-fungsi inti untuk menginisialisasi engine, memuat program IR, menjalankan transformasi, dan melakukan verifikasi. API ini didefinisikan terutama dalam `cxs.h` dan `cxs_emit.h`.

---

## 1. Struktur Data Utama

### `cxs_engine_t`

Struktur ini adalah inti dari engine CXS, menyimpan semua status yang relevan dengan program IR, transformasi, dan verifikasi. Ini mencakup:

-   `insns`: Array instruksi IR.
-   `num_insns`: Jumlah instruksi saat ini.
-   `blocks`: Array blok dasar (basic blocks).
-   `num_blocks`: Jumlah blok dasar.
-   `stats`: Statistik transformasi yang diterapkan.
-   `prng_seed`: Seed untuk Pseudo-Random Number Generator (PRNG).
-   `golden_result`: Hasil eksekusi program asli untuk verifikasi.

### `insn_t`

Merepresentasikan satu instruksi dalam Representasi Perantara (IR). Setiap instruksi memiliki:

-   `type`: Tipe instruksi (lihat `insn_type_t`).
-   `num_ops`: Jumlah operand.
-   `ops`: Array operand (`operand_t`).
-   `block_id`: ID blok dasar tempat instruksi berada.
-   `is_junk`, `is_opaque`: Flag untuk menandai instruksi yang disuntikkan atau bagian dari predikat opak.

### `operand_t`

Merepresentasikan operand dari sebuah instruksi:

-   `type`: Tipe operand (`operand_type_t`).
-   `reg`: ID register abstrak jika operand adalah register.
-   `imm`: Nilai immediate jika operand adalah immediate.
-   `label`: Nama label jika operand adalah label.

### `insn_type_t`

Enumerasi yang mendefinisikan semua jenis instruksi IR yang didukung oleh CXS, termasuk instruksi aritmatika, kontrol alur, dan instruksi pseudo untuk obfuskasi. Contoh:

-   `INSN_ADD`, `INSN_SUB`, `INSN_MOV`
-   `INSN_JMP`, `INSN_CALL`, `INSN_RET`
-   `INSN_JUNK`, `INSN_OVERLAP`, `INSN_STATE_MOV` (untuk T7)

### `reg_id_t`

Enumerasi untuk ID register abstrak, yang kemudian dipetakan ke register fisik sesuai arsitektur target. Contoh:

-   `REG_R0` (rax/x0), `REG_R1` (rbx/x1), ..., `REG_R15` (r15/x15)
-   `REG_STATE` (alias untuk `REG_R13`), `REG_ITGT` (alias untuk `REG_R14`)

---

## 2. Fungsi-fungsi Utama

### Inisialisasi dan Pembersihan

-   `cxs_engine_t* cxs_init(uint64_t prng_seed)`
    -   **Deskripsi**: Menginisialisasi engine CXS baru dengan seed PRNG yang diberikan.
    -   **Parameter**: `prng_seed` (uint64_t) - Seed untuk generator angka acak.
    -   **Mengembalikan**: Pointer ke `cxs_engine_t` yang baru diinisialisasi, atau `NULL` jika gagal.

-   `void cxs_free(cxs_engine_t *e)`
    -   **Deskripsi**: Membebaskan semua sumber daya yang dialokasikan oleh engine CXS.
    -   **Parameter**: `e` (cxs_engine_t*) - Pointer ke engine CXS yang akan dibebaskan.

### Memuat Program IR

-   `int cxs_load_file(cxs_engine_t *e, const char *filename)`
    -   **Deskripsi**: Memuat program IR dari file `.cxs` yang ditentukan.
    -   **Parameter**: `e` (cxs_engine_t*), `filename` (const char*) - Path ke file `.cxs`.
    -   **Mengembalikan**: `CXS_OK` jika berhasil, kode error lain jika gagal.

-   `int cxs_load_sample(cxs_engine_t *e)`
    -   **Deskripsi**: Memuat program sampel bawaan ke dalam engine.
    -   **Parameter**: `e` (cxs_engine_t*).
    -   **Mengembalikan**: `CXS_OK` jika berhasil, kode error lain jika gagal.

### Transformasi dan Verifikasi

-   `void cxs_capture_golden(cxs_engine_t *e)`
    -   **Deskripsi**: Menjalankan program IR asli dan menyimpan hasilnya sebagai "Golden Snapshot" untuk verifikasi.

-   `void cxs_run_pipeline(cxs_engine_t *e)`
    -   **Deskripsi**: Menerapkan semua 30 teknik obfuskasi secara berurutan ke program IR.

-   `int cxs_verify_equivalence(cxs_engine_t *e)`
    -   **Deskripsi**: Memverifikasi kesetaraan semantik program yang diaburkan dengan "Golden Snapshot" menggunakan 32 vektor input.
    -   **Mengembalikan**: `CXS_OK` jika verifikasi berhasil, `CXS_ERR` jika ada ketidaksesuaian.

### Output dan Debugging

-   `void cxs_disasm_print(cxs_engine_t *e)`
    -   **Deskripsi**: Mencetak disassembler dari program IR saat ini ke konsol.

-   `void cxs_stats_print(cxs_engine_t *e)`
    -   **Deskripsi**: Mencetak statistik tentang transformasi yang diterapkan.

### ASM Emitter (dari `cxs_emit.h`)

-   `int cxs_emit_asm(cxs_engine_t *e, const char *filename, int target_arch)`
    -   **Deskripsi**: Menghasilkan file assembly `.S` dari program IR yang diaburkan.
    -   **Parameter**: `e` (cxs_engine_t*), `filename` (const char*) - Nama file output, `target_arch` (int) - Arsitektur target (misalnya `CXS_ARCH_X86_64`, `CXS_ARCH_ARM64`).
    -   **Mengembalikan**: `CXS_OK` jika berhasil, kode error lain jika gagal.

---

## 3. Kode Pengembalian (Return Codes)

CXS menggunakan kode pengembalian standar untuk menunjukkan status operasi:

-   `CXS_OK` (0): Operasi berhasil.
-   `CXS_ERR` (-1): Kesalahan umum.
-   `CXS_OOM` (-2): Kekurangan memori (Out Of Memory).
-   `CXS_BOUNDS` (-3): Batas kapasitas terlampaui.
-   `CXS_NOFILE` (-4): File tidak ditemukan.

---

## Referensi

[1] `cxs.h` - Definisi API, tipe IR, dan engine 30-transformasi.
[2] `cxs_emit.h` - Definisi API untuk emitter assembly.
