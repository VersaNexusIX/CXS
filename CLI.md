# CLI CXS — Code eXecution Scrambler v4.0

Antarmuka Baris Perintah (CLI) adalah cara utama untuk berinteraksi dengan engine CXS. Ini memungkinkan pengguna untuk memuat file, menjalankan obfuskasi, melakukan tes stres, dan menghasilkan file assembly.

---

## 1. Penggunaan Dasar

Eksekusi `cxs` tanpa argumen akan menjalankan demo interaktif yang menggunakan program sampel bawaan. Ini adalah cara yang baik untuk melihat pipeline transformasi beraksi.

```bash
./cxs
```

---

## 2. Opsi dan Argumen

Berikut adalah daftar lengkap opsi yang tersedia di CLI CXS:

| Opsi                 | Deskripsi                                                                    |
| :------------------- | :--------------------------------------------------------------------------- |
| `(tanpa argumen)`    | Menjalankan demo interaktif pada program sampel bawaan.                      |
| `--demo`             | Sama seperti tanpa argumen.                                                  |
| `-f <file.cxs>`      | Mengaburkan file IR target yang ditentukan.                                  |
| `-s N`               | Digunakan bersama `-f`, menjalankan `N` siklus tes stres setelah obfuskasi.  |
| `--stress N`         | Menjalankan tes stres pada sampel bawaan sebanyak `N` siklus.                |
| `--emit-asm`         | Menghasilkan file assembly (`.S`) dari IR yang telah diaburkan.              |
| `-o <file.S>`        | Menentukan nama file output untuk assembly (digunakan bersama `--emit-asm`). |
| `--x86-64` / `--arm64` | Memaksa arsitektur output tertentu saat menggunakan `--emit-asm`.            |
| `--help` / `-h`      | Menampilkan pesan bantuan yang terperinci.                                   |
| `--version`          | Menampilkan informasi versi dan build.                                       |

---

## 3. Contoh Alur Kerja

### Mengaburkan File IR

Untuk mengaburkan file IR kustom, gunakan flag `-f`:

```bash
./cxs -f samples/double.cxs
```

Outputnya akan menampilkan disassembler dari IR asli, log dari setiap transformasi yang diterapkan, statistik, dan hasil verifikasi.

### Menjalankan Tes Stres

Tes stres sangat penting untuk memastikan stabilitas engine obfuskasi. Anda dapat menjalankannya pada file tertentu atau pada sampel bawaan:

```bash
# Menjalankan 20 siklus tes stres pada file IR kustom
./cxs -f samples/triple.cxs -s 20

# Menjalankan 50 siklus tes stres pada sampel bawaan
./cxs --stress 50
```

### Menghasilkan File Assembly

Setelah obfuskasi, Anda dapat menghasilkan file assembly yang dapat dikompilasi menggunakan `--emit-asm`:

```bash
# Menghasilkan file assembly dengan nama otomatis (misalnya, obfuscated_x86_64.S)
./cxs -f samples/double.cxs --emit-asm

# Menghasilkan file assembly dengan nama kustom dan memaksa arsitektur ARM64
./cxs -f samples/double.cxs --emit-asm --arm64 -o my_obfuscated_arm.S
```

---

## 4. Format File IR (.cxs)

CLI CXS mengharapkan file input dalam format `.cxs`, yang merupakan format teks sederhana mirip assembly. File ini terdiri dari blok-blok kode yang diawali dengan `.block` dan diakhiri dengan `.end`.

```asm
# Ini adalah komentar
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

-   **Komentar**: Baris yang diawali dengan `#`.
-   **Blok**: Didefinisikan dengan `.block .nama_label` dan `.end`.
-   **Label**: Didefinisikan dengan `label .nama_label`.
-   **Instruksi**: Menggunakan mnemonik standar (misalnya, `add`, `sub`, `jmp`).

---

## Referensi

[1] `main.c` - Implementasi antarmuka baris perintah (CLI).
[2] `cxs.h` - Definisi API dan struktur data inti.
