# CXS — Code eXecution Scrambler v4.0

**Polymorphic Self-Modifying ASM Engine with 30 Obfuscation Transforms**

```
  ██████╗██╗  ██╗███████╗
 ██╔════╝╚██╗██╔╝██╔════╝
 ██║      ╚███╔╝ ███████╗
 ██║      ██╔██╗ ╚════██║
 ╚██████╗██╔╝ ██╗███████║
  ╚═════╝╚═╝  ╚═╝╚══════╝
```

CXS adalah engine obfuskasi kode canggih yang mengubah program Representasi Perantara (IR) menjadi output yang setara secara semantik namun sangat sulit dianalisis. Ini dicapai melalui pipeline 30 transformasi berlapis dan kemampuan untuk menghasilkan file assembly (`.S`) yang dapat di-assemble.

---

## Dokumentasi Lengkap

Untuk detail lebih lanjut mengenai proyek CXS, silakan merujuk ke dokumen-dokumen berikut:

-   [**Architecture.md**](Architecture.md): Penjelasan mendalam tentang desain arsitektur CXS, komponen inti, dan pipeline transformasi.
-   [**API.md**](API.md): Detail mengenai Application Programming Interface (API) publik CXS, struktur data utama, dan fungsi-fungsi inti.
-   [**Assembly.md**](Assembly.md): Informasi tentang Assembly Emitter, arsitektur output yang didukung, dan struktur file assembly yang dihasilkan.
-   [**CLI.md**](CLI.md): Panduan lengkap penggunaan Command-Line Interface (CLI) CXS, termasuk opsi, argumen, dan contoh alur kerja.
-   [**Security.md**](Security.md): Fokus pada teknik anti-analisis (Layer 4) dan bagaimana CXS meningkatkan ketahanan perangkat lunak terhadap reverse engineering.

---

## Fitur Utama

-   **30 Transformasi Berlapis**: Obfuskasi kode pada level instruksi, data, alur kontrol, dan anti-analisis.
-   **Dukungan Multi-Arsitektur**: Kompatibel dengan x86-64 dan AArch64.
-   **Verifikasi Semantik**: Memastikan fungsionalitas kode tetap terjaga setelah obfuskasi.
-   **ASM Emitter**: Menghasilkan kode assembly yang siap dikompilasi.

---

## Build

CXS menggunakan `Makefile` untuk kompilasi. Sistem akan secara otomatis mendeteksi platform dan arsitektur Anda.

```bash
make              # Kompilasi standar
make DEBUG=1      # Kompilasi dengan simbol debug
make clean        # Membersihkan hasil build
make stress       # Menjalankan tes stres (50 siklus verifikasi)
```

**Target yang Didukung (deteksi otomatis):**
-   `linux x86_64` — GCC/Clang, GAS AT&T
-   `android arm64` — Termux, AArch64 GAS
-   `macos x86_64` / `macos arm64` — Apple Silicon / Intel
-   `windows x64` — MASM (manual)

---

## Usage

```bash
cxs                                   # Demo bawaan: f(x)=(x+5-3)*2
cxs --stress 50                       # Tes stres 50 siklus (semua 30 transformasi)
cxs -f samples/double.cxs            # Obfuskasi file .cxs IR
cxs -f samples/double.cxs -s 20      # Obfuskasi + 20 siklus verifikasi
cxs -f samples/double.cxs --emit-asm           # Hasilkan ASM native (.S)
cxs -f samples/double.cxs --emit-asm --arm64   # Paksa output AArch64
cxs -f samples/double.cxs --emit-asm -o out.S  # Nama file output kustom
cxs --help
cxs --version
```

---

## .cxs IR File Format

CXS memproses file IR dengan sintaks mirip assembly. Contoh:

```asm
# komentar
.block .entry
  label   .entry
  add     rax, 5
  sub     rax, 3
  jmp     .done
.end

.block .done
  label   .done
  ret
.end
```

**Register:** `rax rbx rcx rdx rsi rdi rsp rbp r8..r15` (x86), `x0..x15 sp fp` (AArch64), `r0..r15` (generic)

---

## Verification

Setiap proses obfuskasi diverifikasi untuk kesetaraan semantik:
-   32 vektor input diuji terhadap fungsi emas `f(x)=(x+2)*2`.
-   Tes stres: 30 eksekusi independen dengan seed acak baru.
-   Hasil: **30/30 PASS**, `f(10) = 24` setiap eksekusi.

---

*CXS v4.0 — T1 through T30 — by VersaNexusIX*
