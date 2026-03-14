# SHELL.md — Shell MyOS

## Prompt

```
root@myos:/# _
```

Format: `<user>@<hostname>:<cwd># ` dengan ANSI color — green untuk user, cyan untuk hostname, biru untuk path.

Shell membaca input karakter per karakter via `uart_getc()`. Mendukung:
- **Backspace / DEL** — hapus karakter terakhir
- **Ctrl+C** — batalkan baris saat ini, kembali ke prompt
- **Enter** — eksekusi perintah

---

## Filesystem

### `ls [path]`
Tampilkan isi direktori. Default: direktori saat ini.
```
root@myos:/# ls
bin/  etc/  home/  lib/  tmp/  usr/  var/

root@myos:/# ls /etc
hostname  motd  shells  version
```

### `cd <dir>`
Pindah direktori. Mendukung path absolut dan relatif.
```
root@myos:/# cd /etc
root@myos:/etc# cd ..
root@myos:/#
```

### `pwd`
Tampilkan path direktori saat ini.
```
root@myos:/usr/bin# pwd
/usr/bin
```

### `mkdir <dir>`
Buat direktori baru.
```
root@myos:/# mkdir /home/project
```

### `touch <file>`
Buat file kosong.
```
root@myos:/# touch /tmp/notes.txt
```

### `cat <file>`
Tampilkan isi file.
```
root@myos:/# cat /etc/version
MyOS 1.0 ARM64
```

### `echo <text>`
Cetak teks. Mendukung redirect dengan `>`.
```
root@myos:/# echo Hello World
Hello World

root@myos:/# echo "My config" > /etc/myfile
```

### `write <file> <teks>`
Tulis teks langsung ke file (menimpa isi lama).
```
root@myos:/# write /etc/hostname myserver
```

### `rm <path>`
Hapus file atau direktori.
```
root@myos:/# rm /tmp/notes.txt
```

### `cp <src> <dst>`
Salin file.
```
root@myos:/# cp /etc/hostname /tmp/hostname.bak
```

### `mv <src> <dst>`
Pindahkan / rename file.
```
root@myos:/# mv /tmp/notes.txt /tmp/catatan.txt
```

### `wc <file>`
Hitung baris, kata, dan byte dalam file.
```
root@myos:/# wc /etc/motd
  3  12  64 /etc/motd
```

### `head <file>`
Tampilkan 10 baris pertama file.
```
root@myos:/# head /var/pkg/db
```

### `tail <file>`
Tampilkan 10 baris terakhir file.
```
root@myos:/# tail /var/log/install.log
```

### `stat <file>`
Tampilkan informasi file.
```
root@myos:/# stat /etc/hostname
  File: /etc/hostname
  Type: regular file
Access: (0644/-rw-r--r--)
   Uid: (    0/    root)
```

### `df`
Tampilkan penggunaan filesystem (ramfs).
```
root@myos:/# df
Filesystem    Nodes    Used    Free
ramfs            64       8      56
```

---

## Archive

### `tar x <file> [destdir]`
Extract arsip TAR (format ustar). Jika `destdir` tidak disebut, extract ke direktori saat ini.
```
root@myos:/# tar x /tmp/app.tar /usr/local
tar: extracted 12 file(s)
```

### `tar t <file>`
Tampilkan isi arsip TAR tanpa extract.
```
root@myos:/# tar t /tmp/app.tar
app/
app/main
app/README.md
app/lib/util.so
```

### `zip x <file> [destdir]`
Extract arsip ZIP.
```
root@myos:/# zip x /tmp/pkg.zip /tmp/out
```

### `zip l <file>`
Tampilkan isi arsip ZIP.
```
root@myos:/# zip l /tmp/pkg.zip
```

---

## Network

### `ifconfig`
Tampilkan konfigurasi jaringan saat ini.
```
root@myos:/# ifconfig
eth0: inet 10.0.2.15  netmask 255.255.255.0
      gw 10.0.2.2     mac 52:54:00:12:34:56
```

### `ifconfig <ip> <gateway>`
Set IP address dan gateway.
```
root@myos:/# ifconfig 192.168.1.100 192.168.1.1
```

### `ping <host>`
Kirim ICMP echo request ke host. Host berupa IP address.
```
root@myos:/# ping 10.0.2.2
PING 10.0.2.2
Reply from 10.0.2.2: icmp_seq=1
Reply from 10.0.2.2: icmp_seq=2
Reply from 10.0.2.2: icmp_seq=3
```

### `wget <url> [-O <file>]`
Download file dari URL. Jika `-O` tidak disebut, simpan ke `/tmp/` dengan nama dari URL.
```
root@myos:/# wget http://example.com/data.tar.gz
root@myos:/# wget http://10.0.2.2/app.tar -O /tmp/app.tar
```

### `curl <url> [-o <file>]`
HTTP GET ke URL. Jika `-o` tidak disebut, tampilkan output ke layar.
```
root@myos:/# curl http://10.0.2.2/api/info
root@myos:/# curl http://10.0.2.2/file.txt -o /tmp/file.txt
```

### `netstat`
Tampilkan koneksi TCP aktif.
```
root@myos:/# netstat
Active TCP connections:
  fd=0  10.0.2.15:49152  ->  93.184.216.34:80  ESTABLISHED
```

---

## Process

### `ps`
Tampilkan tabel proses.
```
root@myos:/# ps
  PID  STATE    NAME
    0  RUNNING  kernel
    1  READY    shell
```

### `kill <pid>`
Hentikan proses berdasarkan PID.
```
root@myos:/# kill 3
```

### `exec <file> [args...]`
Load dan jalankan binary ELF64 ARM64.
```
root@myos:/# exec /usr/bin/hello
root@myos:/# exec /usr/bin/myapp --verbose
```

Shell juga secara otomatis mencari binary di `/usr/bin/` — jadi jika `nano` sudah diinstall via `mos`, cukup ketik `nano` langsung.

---

## Package Manager

Lihat [PACKAGES.md](PACKAGES.md) untuk dokumentasi lengkap. Ringkasan:

```bash
mos -i nano                    # install dari registry
mos -i sharkdp/bat             # install dari GitHub
mos -i cli/cli@v2.40.0         # GitHub tagged release
mos -i https://example.com/x.tar.gz  # install dari URL
mos -r nano                    # hapus
mos -u nano                    # upgrade
mos -U                         # upgrade semua
mos -l                         # list installed
mos -a                         # list tersedia
mos -s <query>                 # cari
mos -I nano                    # info paket
mos -c nano                    # cek integritas file
mos --hold nano                # tahan versi
mos --clean                    # bersihkan cache
```

---

## System

### `uname [-a]`
Informasi sistem. Flag `-a` untuk detail lengkap.
```
root@myos:/# uname
MyOS

root@myos:/# uname -a
MyOS 1.0.0 myos 2026 aarch64 ARM64
```

### `hostname`
Tampilkan hostname (dibaca dari `/etc/hostname`).
```
root@myos:/# hostname
myos
```

### `uptime`
Tampilkan waktu sejak boot.
```
root@myos:/# uptime
up 0 days, 0 hours, 3 minutes
```

### `free`
Tampilkan penggunaan memori heap.
```
root@myos:/# free
Heap start : 0x40800000
Heap size  : 16777216 bytes (16 MiB)
Used       : 204800 bytes
Free       : 16572416 bytes
```

### `date`
Tampilkan tanggal dan waktu.
```
root@myos:/# date
MyOS build: 2026 (no RTC)
```

### `env`
Tampilkan variabel environment.
```
root@myos:/# env
PATH=/usr/bin:/bin:/usr/local/bin
HOME=/root
USER=root
SHELL=mysh
TERM=vt100
```

### `clear`
Bersihkan layar (ANSI escape `\x1b[2J\x1b[H`).

### `reboot`
Restart sistem.
```
root@myos:/# reboot
Broadcast message: System is going down for reboot NOW!
```

### `halt` / `poweroff`
Hentikan sistem.
```
root@myos:/# halt
System halted.
```

### `help`
Tampilkan daftar semua perintah.

---

## Error & "Did You Mean?"

Saat perintah tidak ditemukan, shell memberikan saran cerdas:

```
root@myos:/# sl
Error: sl: command not found
Did you mean "ls"?

root@myos:/# apt install vim
Error: apt: command not found
Did you mean "mos"? (use 'mos -i <pkg>' to install)

root@myos:/# ipconfig
Error: ipconfig: command not found
Did you mean "ifconfig"?

root@myos:/# shutdwon
Error: shutdwon: command not found
Did you mean "shutdown"? (or use 'reboot')

root@myos:/# gti status
Error: gti: command not found
Did you mean "git"? (install with: mos -i git)
```

**Cara kerjanya:**

1. Cek tabel typo eksplisit (50+ pasangan: `sl→ls`, `apt→mos`, `ipconfig→ifconfig`, `quit→halt`, dll)
2. Kalau tidak cocok, hitung **Levenshtein distance** antara input dan semua perintah built-in
3. Jika jarak ≤ 2, tampilkan saran
4. Jika tidak ada saran, tampilkan:
   ```
   Try:
     mos -s <cmd>   (search packages)
     mos -i <cmd>   (try install from registry)
     help           (list all commands)
   ```

**Tabel typo yang dikenali:**

| Ketik | Saran | Hint |
|-------|-------|------|
| `sl` | `ls` | — |
| `dir`, `DIR` | `ls` | — |
| `man` | `help` | use 'help' or 'help \<cmd\>' |
| `type`, `more`, `less` | `cat` | — |
| `del`, `erase` | `rm` | — |
| `copy` | `cp` | — |
| `move`, `rename` | `mv` | — |
| `ipconfig`, `ifconf` | `ifconfig` | — |
| `apt`, `apt-get`, `apk` | `mos` | use 'mos -i \<pkg\>' |
| `pacman`, `yum`, `dnf` | `mos` | use 'mos -i \<pkg\>' |
| `brew`, `pip`, `npm` | `mos` | — |
| `pkg`, `install` | `mos` | — |
| `cargo` | `mos` | use 'mos -i \<owner\>/\<repo\>' |
| `exit`, `quit`, `logout` | `halt` | or 'reboot' |
| `shutdown`, `powerdown` | `halt` | — |
| `top` | `ps` | or install htop |
| `vim`, `nano`, `vi` | `exec` | install with mos -i |
| `git`, `python`, `python3` | `exec` | install with mos -i |
| `bash`, `zsh` | `exec` | install with mos -i |
