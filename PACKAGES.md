# PACKAGES.md — Package Manager `mos`

`mos` adalah package manager bawaan MyOS. Nama "mos" adalah singkatan dari **MyOS Software**.

---

## Format Instalasi

`mos` mendukung empat cara install:

```
mos -i <nama>                          # dari registry bawaan
mos -i <owner>/<repo>                  # dari GitHub (branch default)
mos -i <owner>/<repo>@<tag>            # dari GitHub (tag/release tertentu)
mos -i https://example.com/pkg.tar.gz  # dari URL langsung
```

---

## Perintah Lengkap

| Perintah | Keterangan |
|----------|-----------|
| `mos -i <spec>` | Install paket |
| `mos -r <nama>` | Hapus paket |
| `mos -u <nama>` | Upgrade paket ke versi terbaru |
| `mos -U` | Upgrade semua paket yang terinstall |
| `mos -R <nama>` | Reinstall paket |
| `mos -l` | Daftar paket yang terinstall |
| `mos -a` | Daftar semua paket di registry |
| `mos -s <query>` | Cari paket |
| `mos -I <nama>` | Info detail paket |
| `mos -c <nama>` | Cek integritas file paket |
| `mos --hold <nama>` | Tahan paket di versi saat ini |
| `mos --unhold <nama>` | Lepas hold |
| `mos --clean` | Bersihkan cache download |

---

## Contoh Penggunaan

```bash
# Install dari registry
mos -i nano
mos -i htop
mos -i jq

# Install dari GitHub — menggunakan branch default
mos -i sharkdp/bat
mos -i BurntSushi/ripgrep
mos -i VersaNexusIX/HXD

# Install dari GitHub — tag spesifik
mos -i cli/cli@v2.40.0
mos -i neovim/neovim@v0.10.0
mos -i junegunn/fzf@0.44.1

# Install dari URL
mos -i https://example.com/myapp-aarch64.tar.gz
mos -i https://releases.example.com/tool-1.0.zip

# Manajemen
mos -l                  # lihat yang terinstall
mos -I sharkdp/bat      # info + versi + source
mos -u bat              # upgrade ke versi terbaru
mos -r bat              # hapus
mos -c bat              # verifikasi file masih ada

# Cari
mos -s editor           # cari di registry
mos -s json
```

---

## Alur Install GitHub (`mos -i owner/repo`)

```
mos -i VersaNexusIX/HXD
         │
         ▼
[1] Parse spec
    owner = "VersaNexusIX"
    repo  = "HXD"
    ref   = "" (ambil terbaru)
         │
         ▼
[2] Resolve default branch
    GET api.github.com/repos/VersaNexusIX/HXD
    → "default_branch": "main"
         │
         ▼
[3] Cari file mos.pkg di repo
    GET raw.githubusercontent.com/VersaNexusIX/HXD/main/mos.pkg
    GET raw.githubusercontent.com/VersaNexusIX/HXD/main/.mos.pkg
    GET raw.githubusercontent.com/VersaNexusIX/HXD/main/pkg/mos.pkg
    → jika ditemukan: parse metadata, deps, configure args
         │
         ▼
[4] GitHub Releases API
    GET api.github.com/repos/VersaNexusIX/HXD/releases/latest
    → cari release asset ARM64 (.tar.gz dengan "aarch64"/"arm64" di nama)
    → jika ada asset ARM64: pakai langsung (prebuilt binary)
    → jika tidak ada: lanjut ke langkah 5
         │
         ▼
[5] Download source tarball
    GET codeload.github.com/VersaNexusIX/HXD/tar.gz/refs/heads/main
    → simpan ke /var/pkg/cache/VersaNexusIX-HXD-main.tar.gz
         │
         ▼
[6] Extract
    → /var/pkg/src/VersaNexusIX-HXD/
         │
         ▼
[7] Install dependency (jika ada di mos.pkg)
    → rekursif panggil pkg_install() untuk tiap dep
         │
         ▼
[8] Deteksi build system
    scan file di direktori source:
    CMakeLists.txt  → cmake
    configure.ac    → autoconf
    configure       → autoconf (pre-generated)
    meson.build     → meson
    Cargo.toml      → cargo (Rust)
    go.mod          → go build
    setup.py        → python setup.py
    Makefile        → make
    binary saja     → prebuilt
         │
         ▼
[9] Generate build script
    → /var/pkg/build/build-HXD.sh
    (berisi perintah build yang tepat untuk build system yang terdeteksi)
         │
         ▼
[10] Install binary prebuilt (jika ada)
     → copy ke /usr/bin/HXD
         │
         ▼
[11] Tulis manifest
     → /var/pkg/HXD.manifest
         │
         ▼
[12] Simpan database
     → /var/pkg/db
         │
         ▼
==> Successfully installed HXD (main)
```

---

## Format File `mos.pkg`

Kalau repository GitHub kamu ingin mendukung `mos` secara native, buat file `mos.pkg` di root repo:

```ini
# mos.pkg — MyOS package spec
name=myapp
version=2.1.0
desc=My awesome application for MyOS
url=https://github.com/me/myapp/releases/download/v2.1.0/myapp-aarch64.tar.gz
prefix=/usr/local
configure=--enable-fast --with-ssl --disable-docs
make_args=ARCH=aarch64
dep=openssl
dep=zlib
build_dep=cmake
build_dep=make
```

| Field | Keterangan |
|-------|-----------|
| `name` | Nama paket |
| `version` | Versi |
| `desc` | Deskripsi singkat |
| `url` | URL download langsung (opsional, override auto-detect) |
| `prefix` | Install prefix (default: `/usr`) |
| `configure` | Extra args untuk `./configure` atau `cmake` |
| `make_args` | Extra args untuk `make` |
| `dep` | Runtime dependency (bisa lebih dari satu baris) |
| `build_dep` | Build-time dependency |

---

## Deteksi Build System

`mos` otomatis mendeteksi cara build dari file yang ada di source:

| File ditemukan | Build system | Command yang dihasilkan |
|----------------|-------------|------------------------|
| `CMakeLists.txt` | CMake | `cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && make install` |
| `configure.ac` | Autoconf | `autoreconf -fiv && ./configure --prefix=/usr && make && make install` |
| `configure` | Autoconf (pre-generated) | `./configure --prefix=/usr && make && make install` |
| `meson.build` | Meson | `meson setup _build && ninja -C _build && ninja -C _build install` |
| `Cargo.toml` | Cargo (Rust) | `cargo build --release && cp target/release/... /usr/bin/` |
| `go.mod` | Go | `go build -o /usr/bin/ ./...` |
| `setup.py` | Python | `python3 setup.py install --prefix=/usr` |
| `Makefile` | Make | `make -j$(nproc) && make install PREFIX=/usr` |
| binary saja | Prebuilt | copy langsung ke `/usr/bin/` |

> **Catatan:** MyOS tidak punya compiler bawaan. Build script yang di-generate tersedia di `/var/pkg/build/build-<nama>.sh` untuk dijalankan di lingkungan Linux atau cross-compile environment.

---

## Registry Bawaan

Paket berikut bisa diinstall langsung dengan `mos -i <nama>`:

| Nama | GitHub | Keterangan |
|------|--------|-----------|
| `nano` | nicowillis/nano | Text editor sederhana |
| `vim` | vim/vim | Vi Improved |
| `neovim` / `nvim` | neovim/neovim | Neovim |
| `htop` | htop-dev/htop | Process viewer interaktif |
| `curl` | curl/curl | HTTP client CLI |
| `git` | git/git | Version control |
| `busybox` | mirror/busybox | Swiss-army knife embedded |
| `jq` | jqlang/jq | JSON processor |
| `fzf` | junegunn/fzf | Fuzzy finder |
| `bat` | sharkdp/bat | cat dengan syntax highlight |
| `fd` | sharkdp/fd | find yang lebih cepat |
| `ripgrep` / `rg` | BurntSushi/ripgrep | grep yang sangat cepat |
| `exa` | ogham/exa | ls modern |
| `lsd` | lsd-rs/lsd | ls dengan icon |
| `tmux` | tmux/tmux | Terminal multiplexer |
| `zsh` | zsh-users/zsh | Zsh shell |
| `fish` | fish-shell/fish | Fish shell |
| `starship` | starship-rs/starship | Prompt minimalis |
| `make` | mirror/make | GNU Make |
| `cmake` | Kitware/CMake | Build system |
| `gcc` | gcc-mirror/gcc | GNU Compiler Collection |
| `clang` | llvm/llvm-project | LLVM/Clang |
| `python3` | python/cpython | Python 3 |
| `lua` | lua/lua | Lua scripting |
| `sqlite` | sqlite/sqlite | SQLite database |
| `openssl` | openssl/openssl | TLS/crypto library |
| `wget` | mirror/wget | Downloader |
| `strace` | strace/strace | Syscall tracer |
| `gdb` | bminor/gdb | GNU Debugger |
| `valgrind` | valgrind-project/valgrind | Memory checker |
| `nmap` | nmap/nmap | Network scanner |
| `ncurses` | mirror/ncurses | TUI library |
| `zlib` | madler/zlib | Compression library |

---

## Database Paket

Database tersimpan di `/var/pkg/db` dalam format teks sederhana:

```
pkg=bat
ver=v0.24.0
src=https://github.com/sharkdp/bat/archive/v0.24.0.tar.gz
sta=1
---
pkg=jq
ver=jq-1.7.1
src=https://github.com/jqlang/jq/archive/jq-1.7.1.tar.gz
sta=1
---
```

Field `sta`: `0` = none, `1` = installed, `2` = broken, `3` = held.

Manifest per paket tersimpan di `/var/pkg/<nama>.manifest`:
```
name=bat
version=v0.24.0
source=github:sharkdp/bat
ref=v0.24.0
build=cargo (Rust)
```

---

## Direktori Layout

```
/var/pkg/
├── db                    # database semua paket terinstall
├── cache/                # arsip yang didownload (.tar.gz, .zip)
│   └── sharkdp-bat-v0.24.0.tar.gz
├── src/                  # source yang sudah di-extract
│   └── sharkdp-bat/
├── build/                # generated build scripts
│   └── build-bat.sh
├── log/                  # log instalasi
└── bat.manifest          # manifest file per paket
```

---

## Status Paket

| Status | Kode | Keterangan |
|--------|------|-----------|
| `PKG_STATE_NONE` | 0 | Tidak terinstall |
| `PKG_STATE_INSTALLED` | 1 | Terinstall normal |
| `PKG_STATE_BROKEN` | 2 | Gagal install / file hilang |
| `PKG_STATE_HELD` | 3 | Ditahan, tidak bisa di-upgrade/remove |
