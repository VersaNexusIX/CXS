# Assembly CXS — Code eXecution Scrambler v4.0

Modul Assembly Emitter di CXS bertanggung jawab untuk mengubah Representasi Perantara (IR) yang telah diaburkan menjadi kode assembly asli yang dapat dikompilasi. Ini adalah langkah krusial untuk menghasilkan output yang dapat dieksekusi dari proses obfuskasi.

---

## 1. Fungsi dan Tujuan

Assembly Emitter memiliki tujuan utama:

-   **Konversi IR ke Assembly**: Menerjemahkan instruksi IR internal CXS ke sintaks assembly spesifik arsitektur (GAS AT&T untuk x86-64 dan AArch64).
-   **Dukungan Multi-Arsitektur**: Menghasilkan kode assembly yang sesuai untuk target `x86-64` dan `AArch64`.
-   **Anotasi Transformasi**: Setiap instruksi assembly yang dihasilkan dianotasi dengan komentar yang menunjukkan teknik obfuskasi mana yang bertanggung jawab atas keberadaannya atau modifikasinya. Ini sangat membantu untuk analisis dan debugging.
-   **File Output Mandiri**: Menghasilkan file `.S` yang lengkap dan valid, siap untuk di-assemble oleh GNU Assembler (GAS) atau dikompilasi oleh GCC/Clang.

---

## 2. Arsitektur Output yang Didukung

Emitter mendukung dua arsitektur utama, dengan kemampuan deteksi otomatis atau penentuan paksa:

| Target Emitter    | Deskripsi                                   | Sintaks Assembly | Sistem Operasi yang Didukung |
| :---------------- | :------------------------------------------ | :--------------- | :--------------------------- |
| `CXS_EMIT_X86_64` | Output paksa untuk arsitektur x86-64.     | GAS AT&T         | Linux, macOS, BSD            |
| `CXS_EMIT_ARM64`  | Output paksa untuk arsitektur AArch64.    | GAS AArch64      | Android, iOS, Apple Silicon  |
| `CXS_EMIT_NATIVE` | Deteksi otomatis berdasarkan arsitektur host. | Sesuai deteksi   | Sesuai deteksi               |

---

## 3. Penggunaan Emitter

Fungsi utama untuk menghasilkan file assembly adalah `cxs_emit_asm()`, yang mengambil objek `cxs_engine_t` dan opsi emitter sebagai parameter.

### `cxs_emit_asm(cxs_engine_t *e, const cxs_emit_opts_t *opts)`

-   **Deskripsi**: Mengeluarkan status engine saat ini sebagai file sumber assembly.
-   **Parameter**:
    -   `e` (`cxs_engine_t*`): Pointer ke engine CXS yang berisi IR yang telah diaburkan.
    -   `opts` (`const cxs_emit_opts_t*`): Struktur yang berisi opsi untuk emitter.
-   **Mengembalikan**: `CXS_OK` jika berhasil, `CXS_ERR` jika ada kegagalan I/O.

### Struktur `cxs_emit_opts_t`

Struktur ini memungkinkan konfigurasi perilaku emitter:

-   `target` (`cxs_emit_target_t`): Menentukan arsitektur target output (misalnya `CXS_EMIT_X86_64`).
-   `outfile` (`const char*`): Nama file output. Jika `NULL`, nama file akan dibuat secara otomatis (misalnya `obfuscated_x86_64.S`).
-   `fn_name` (`const char*`): Nama simbol fungsi yang diekspor dalam file assembly (default: `cxs_fn`).
-   `annotate` (`int`): Jika `1`, instruksi akan dianotasi dengan komentar transformasi.
-   `show_stats` (`int`): Jika `1`, blok komentar statistik akan ditambahkan di awal file.

### Contoh Penggunaan CLI

```bash
# Menghasilkan assembly untuk file IR dan arsitektur otomatis
cxs -f samples/double.cxs --emit-asm

# Menghasilkan assembly dan memaksa output AArch64
cxs -f samples/double.cxs --emit-asm --arm64

# Menghasilkan assembly ke nama file kustom
cxs -f samples/double.cxs --emit-asm -o my_obfuscated_code.S
```

---

## 4. Struktur File Assembly yang Dihasilkan

File `.S` yang dihasilkan oleh CXS akan memiliki struktur umum sebagai berikut:

```assembly
.section .text
.global cxs_fn

cxs_fn:
    ; ... Instruksi assembly yang diaburkan ...
    ; Setiap instruksi dianotasi dengan sumber transformasinya, contoh:
    xorq     $4, %r14         // [T14] decrypt stub
    movq     $670400367, %r14 // [T9]  indirect jump
    movl     $1, %eax         // [T15] CPUID leaf 1
    lsl      x8, x8, #3       // [T30] key schedule

    ; ... Blok-blok khusus untuk transformasi tertentu (misalnya T14, T7) ...

.decrypt_stub:
    ; ... Kode untuk stub dekripsi polimorfik (jika T14 diterapkan) ...

.cff_loop:
    ; ... Dispatcher untuk Control Flow Flattening (jika T7 diterapkan) ...

.cff_b0:
    ; ... Blok dasar yang diatur ulang ...

.cff_done:
    ret
```

---

## 5. Kompilasi File Assembly

File `.S` yang dihasilkan dapat dikompilasi menggunakan assembler atau compiler standar:

```bash
# Meng-assemble menggunakan GAS
as -o out.o obfuscated_x86_64.S

# Mengkompilasi menggunakan GCC/Clang
cc -c -o out.o obfuscated_x86_64.S
```

---

## Referensi

[1] `cxs_emit.h` - Definisi API untuk emitter assembly.
[2] `emit.c` - Implementasi emitter assembly x86-64 dan AArch64 GAS.
