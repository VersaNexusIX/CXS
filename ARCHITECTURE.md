# Arsitektur CXS — Code eXecution Scrambler v4.0

CXS (Code eXecution Scrambler) adalah engine obfuskasi kode yang canggih, dirancang untuk mengubah representasi perantara (IR) menjadi kode assembly yang sangat sulit dianalisis, namun tetap mempertahankan fungsionalitas aslinya. Arsitektur CXS berpusat pada pipeline transformasi berlapis dan sistem verifikasi semantik.

---

## 1. Komponen Utama

Proyek CXS terdiri dari beberapa komponen inti yang bekerja sama untuk mencapai tujuan obfuskasi:

- **CLI (Command-Line Interface)**: Antarmuka utama bagi pengguna untuk berinteraksi dengan CXS, memuat file IR, menjalankan transformasi, dan menghasilkan output assembly.
- **Loader**: Bertanggung jawab untuk membaca file `.cxs` (format IR kustom) dan mengonversinya menjadi struktur data IR internal yang dapat dimanipulasi oleh engine.
- **Engine Inti**: Mesin utama yang mengelola pipeline transformasi, disassembler, verifikator, dan generator angka acak (PRNG).
- **Transformasi (T1-T30)**: Kumpulan 30 teknik obfuskasi yang dibagi menjadi empat lapisan, masing-masing menargetkan aspek kode yang berbeda (instruksi, data, alur kontrol, anti-analisis).
- **Verifier**: Sistem yang memastikan bahwa kode yang telah diaburkan secara semantik setara dengan kode aslinya, menggunakan VM internal dan pengujian vektor input.
- **ASM Emitter**: Modul yang bertanggung jawab untuk mengonversi IR yang telah diaburkan menjadi kode assembly asli (GAS AT&T untuk x86-64 atau AArch64 untuk ARM64).

---

## 2. Pipeline Transformasi

Proses obfuskasi di CXS mengikuti pipeline yang terstruktur, memastikan setiap transformasi diterapkan secara berurutan dan hasilnya diverifikasi:

1.  **Input IR**: Kode sumber dalam format `.cxs` dimuat dan diubah menjadi struktur data IR internal.
2.  **Golden Snapshot**: Sebelum transformasi dimulai, CXS membuat 
snapshot "emas" dari program IR asli. Snapshot ini digunakan nanti untuk verifikasi semantik.
3.  **Aplikasi Transformasi**: Engine secara iteratif menerapkan 30 teknik obfuskasi. Setiap transformasi dapat memodifikasi instruksi, operand, atau alur kontrol program.
4.  **Verifikasi Semantik**: Setelah semua transformasi diterapkan, verifikator menjalankan kode yang diaburkan pada serangkaian vektor input dan membandingkan hasilnya dengan "Golden Snapshot". Jika ada ketidaksesuaian, obfuskasi dianggap gagal.
5.  **Output Assembly (Opsional)**: Jika diminta, IR yang telah diaburkan akan diubah menjadi file assembly `.S` yang dapat dikompilasi oleh assembler standar.

```mermaid
graph TD
    A[File .cxs] --> B(Loader)
    B --> C{Internal IR Representation}
    C --> D[Golden Snapshot (for Verification)]
    C --> E[30 Transformasi Berlapis]
    E --> F{Obfuscated IR}
    F --> G(Verifier)
    G -- PASS --> H[ASM Emitter (Optional)]
    G -- FAIL --> I[Error: Semantic Mismatch]
    H --> J[File .S (Assembly)]
```

*Diagram 1: Pipeline Arsitektur CXS*

---

## 3. Representasi Perantara (IR)

CXS menggunakan representasi perantara internal yang agnostik terhadap arsitektur, memungkinkan teknik obfuskasi diterapkan secara universal sebelum dikonversi ke assembly spesifik. IR ini terdiri dari instruksi dan operand:

-   **Instruksi**: Didefinisikan oleh `insn_type_t` dalam `cxs.h`, mencakup operasi aritmatika, kontrol alur, dan instruksi pseudo untuk obfuskasi.
-   **Operand**: Didefinisikan oleh `operand_type_t`, yang dapat berupa register, nilai immediate, alamat memori, atau label.
-   **Register Abstrak**: CXS menggunakan ID register abstrak (`REG_R0` hingga `REG_R15`) yang kemudian dipetakan ke register fisik (misalnya, `rax` untuk x86-64, `x0` untuk AArch64) oleh ASM Emitter.

---

## 4. Dukungan Arsitektur

CXS dirancang untuk mendukung berbagai arsitektur target, dengan deteksi otomatis saat kompilasi:

| Arsitektur     | Sistem Operasi yang Didukung | Assembler/Compiler | Catatan                                   |
| :------------- | :--------------------------- | :----------------- | :---------------------------------------- |
| `x86-64`       | Linux, macOS, BSD, Windows   | GCC/Clang (GAS AT&T), MASM (Windows) | Deteksi otomatis, kecuali Windows (manual) |
| `AArch64`      | Android, iOS, Apple Silicon  | AArch64 GAS        | Deteksi otomatis                           |
| `Generic`      | N/A                          | Pure-C fallback    | Digunakan jika arsitektur tidak terdeteksi |

---

## 5. Mekanisme Verifikasi

Verifikasi adalah bagian krusial dari CXS untuk menjamin bahwa obfuskasi tidak merusak fungsionalitas program. Proses ini melibatkan:

-   **Golden Snapshot**: Menyimpan hasil eksekusi program IR asli untuk serangkaian input.
-   **VM Internal**: CXS memiliki Virtual Machine internal yang dapat mengeksekusi IR yang telah diaburkan.
-   **Pengujian Vektor Input**: Program yang diaburkan dijalankan dengan 32 vektor input yang berbeda, dan outputnya dibandingkan dengan "Golden Snapshot". Jika semua output cocok, verifikasi berhasil.

---

## Referensi

[1] `cxs.h` - Definisi API, tipe IR, dan engine 30-transformasi.
[2] `main.c` - Implementasi antarmuka baris perintah (CLI).
[3] `engine.c` - Implementasi VM, disassembler, verifikator, dan PRNG.
[4] `transform.c` & `transform2.c` - Implementasi teknik obfuskasi T1-T30.
[5] `emit.c` - Implementasi emitter assembly x86-64 dan AArch64 GAS.
