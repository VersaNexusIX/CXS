# NETWORK.md — Network Stack MyOS

## Arsitektur

```
Aplikasi (shell, pkg, http)
         │
         ▼
┌────────────────────────┐
│     HTTP Client        │  net/http.c
│  GET, download, parse  │
└────────────────────────┘
         │
         ▼
┌────────────────────────┐
│     TCP Stack          │  net/net.c
│  connect/send/recv     │
│  state machine         │
└────────────────────────┘
         │
         ▼
┌────────────────────────┐
│     IP Layer           │  net/net.c
│  checksum, routing     │
└────────────────────────┘
         │
         ▼
┌────────────────────────┐
│     Ethernet           │  net/net.c
│  framing, ARP          │
└────────────────────────┘
         │
         ▼
┌────────────────────────┐
│   VirtIO-NET Driver    │  net/net.c
│  MMIO @ 0x0A000000     │
└────────────────────────┘
         │
    ─────┴─────
    QEMU virtio
    ─────┬─────
         │
    Host Network (NAT)
```

---

## VirtIO-NET Driver

### MMIO Registers

| Alamat | Register | Keterangan |
|--------|----------|-----------|
| `0x0A000000` | VIRTIO_MMIO_MAGIC | Magic value `0x74726976` |
| `0x0A000004` | VIRTIO_MMIO_VERSION | Versi |
| `0x0A000008` | VIRTIO_MMIO_DEVICE_ID | Device ID (1 = network) |
| `0x0A000070` | VIRTIO_MMIO_QUEUE_SEL | Pilih virtqueue |
| `0x0A000080` | VIRTIO_MMIO_QUEUE_NUM | Ukuran queue |
| `0x0A000200+` | Config space | MAC address, dll |

### Konfigurasi Default

```
IP Address  : 10.0.2.15
Gateway     : 10.0.2.2
Netmask     : 255.255.255.0
MAC         : 52:54:00:12:34:56
VirtQ size  : 16 slot
MTU         : 1514 bytes
```

> QEMU NAT otomatis assign IP `10.0.2.x` melalui DHCP internal. MyOS menggunakan IP statis karena belum ada DHCP client.

### Ubah IP via Shell

```bash
root@myos:/# ifconfig 192.168.100.10 192.168.100.1
```

---

## TCP Stack

### State Machine

```
CLOSED
  │  tcp_connect() dipanggil
  ▼
SYN_SENT ──── timeout/RST ──→ CLOSED
  │  menerima SYN+ACK
  ▼
ESTABLISHED ◄─────────────────────────
  │                                   │
  │  tcp_send()    tcp_recv()         │
  │  tcp_poll_recv()                  │
  │                                   │
  │  tcp_close() dipanggil            │
  ▼                                   │
FIN_WAIT ──── menerima FIN+ACK ───────┘
  │
  ▼
TIME_WAIT
  │  delay
  ▼
CLOSED
```

### Konstanta

| Konstanta | Nilai | Keterangan |
|-----------|-------|-----------|
| `MAX_TCP_CONNS` | 8 | Maksimum koneksi simultan |
| RX buffer per conn | 8192 bytes | Buffer terima per koneksi |
| TX buffer per conn | 8192 bytes | Buffer kirim per koneksi |

### API TCP

```c
// Buka koneksi ke ip:port, return fd (0–7) atau -1 jika gagal
int tcp_connect(uint32_t ip, uint16_t port);

// Kirim data ke koneksi fd
int tcp_send(int fd, const uint8_t *data, uint16_t len);

// Terima data dari koneksi fd (non-blocking)
int tcp_recv(int fd, uint8_t *buf, uint16_t maxlen);

// Terima data dengan timeout (milidetik)
int tcp_poll_recv(int fd, uint8_t *buf, uint16_t maxlen, int timeout_ms);

// Tutup koneksi
void tcp_close(int fd);
```

### Contoh Penggunaan di C

```c
#include "net.h"

// Koneksi ke 93.184.216.34:80
uint32_t ip = ip_parse("93.184.216.34");
int fd = tcp_connect(ip, 80);
if (fd < 0) {
    // koneksi gagal
    return;
}

// Kirim HTTP request
const char *req = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
tcp_send(fd, (uint8_t*)req, kstrlen(req));

// Terima response dengan timeout 5000ms
uint8_t buf[4096];
int n = tcp_poll_recv(fd, buf, sizeof(buf), 5000);
if (n > 0) {
    buf[n] = 0;
    uart_puts((char*)buf);
}

tcp_close(fd);
```

---

## IP Layer

### API

```c
// Parse string IP "a.b.c.d" → uint32_t (big-endian)
uint32_t ip_parse(const char *s);

// Konversi uint32_t → string "a.b.c.d"
void ip_to_str(uint32_t ip, char *buf);

// Hitung IP checksum (one's complement)
uint16_t ip_checksum(void *data, int len);
```

### Protokol yang Didukung

| Protokol | Kode | Status |
|----------|------|--------|
| TCP | `IP_PROTO_TCP = 6` | ✅ Lengkap |
| ICMP | `IP_PROTO_ICMP = 1` | ✅ Echo request/reply |
| UDP | `IP_PROTO_UDP = 17` | ⚠️ Parsing saja, belum TX |
| ARP | `ETH_TYPE_ARP` | ✅ Request + reply |

---

## HTTP Client

### API

```c
// HTTP GET ke URL, isi response ke struct resp
// Return: HTTP status code (200, 404, ...) atau -1 jika error
int http_get(const char *url, http_response_t *resp);

// HTTP GET dan simpan body langsung ke file
// Return: ukuran file yang disimpan, atau -1
int http_get_to_file(const char *url, const char *dest_path);

// Bebaskan buffer internal response
void http_free_response(http_response_t *resp);

// Parse URL menjadi komponen: host, port, path
int url_parse(const char *url, char *host, uint16_t *port, char *path);
```

### Struct Response

```c
typedef struct {
    int    status;               // HTTP status code (200, 404, ...)
    char   content_type[128];    // "text/html", "application/json", ...
    int    content_length;       // dari Content-Length header
    int    chunked;              // 1 jika Transfer-Encoding: chunked
    char  *body;                 // pointer ke body dalam _raw
    int    body_len;             // panjang body
    char   _raw[65536];          // buffer internal (64 KB)
} http_response_t;
```

### Alur HTTP GET

```
http_get("http://api.github.com/repos/sharkdp/bat")
    │
    ├─ url_parse() → host="api.github.com", port=80, path="/repos/..."
    │
    ├─ ip_parse(host)   [DNS belum ada, perlu IP langsung atau resolve manual]
    │
    ├─ tcp_connect(ip, 80)
    │
    ├─ tcp_send() → "GET /repos/... HTTP/1.1\r\nHost: api.github.com\r\n..."
    │
    ├─ tcp_poll_recv() dengan timeout
    │
    ├─ parse response headers
    │   ├─ status line: "HTTP/1.1 200 OK"
    │   ├─ Content-Type
    │   ├─ Content-Length
    │   └─ Transfer-Encoding: chunked
    │
    ├─ extract body
    │
    └─ tcp_close()
```

### Contoh Penggunaan

```c
#include "http.h"
#include "mm.h"

// Allocate di heap (resp cukup besar, 65KB+)
http_response_t *resp = kmalloc(sizeof(http_response_t));

int status = http_get("http://10.0.2.2/api/data", resp);
if (status == 200 && resp->body) {
    uart_puts(resp->body);
}
kfree(resp);

// Atau download langsung ke file
http_get_to_file("http://10.0.2.2/pkg.tar.gz", "/var/pkg/cache/pkg.tar.gz");
```

---

## Konfigurasi Jaringan via Shell

```bash
# Lihat konfigurasi saat ini
root@myos:/# ifconfig
eth0: inet 10.0.2.15  netmask 255.255.255.0
      gw 10.0.2.2     mac 52:54:00:12:34:56

# Set IP dan gateway baru
root@myos:/# ifconfig 192.168.1.50 192.168.1.1

# Tes konektivitas
root@myos:/# ping 10.0.2.2

# Download file
root@myos:/# wget http://10.0.2.2/myapp.tar.gz -O /tmp/myapp.tar.gz

# HTTP request ke API
root@myos:/# curl http://10.0.2.2/api/status

# Lihat koneksi aktif
root@myos:/# netstat
```

---

## DNS

MyOS belum memiliki DNS resolver. Semua host di HTTP/TCP harus berupa **IP address**. Untuk koneksi ke GitHub, `mos` menggunakan hardcoded IP atau perlu konfigurasi nameserver manual.

> Ini adalah area pengembangan berikutnya: implementasi DNS resolver sederhana (query UDP port 53 ke `10.0.2.3` yang merupakan DNS default QEMU NAT).

---

## QEMU Network Setup

`make run` menjalankan QEMU dengan flag:

```
-netdev user,id=net0 \
-device virtio-net-device,netdev=net0
```

Mode `user` QEMU memberikan:
- IP guest: `10.0.2.15`
- Gateway: `10.0.2.2`
- DNS: `10.0.2.3`
- DHCP: `10.0.2.2`
- Host reachable via: `10.0.2.2`
- Internet: tersedia (via NAT dari host)

Untuk forward port dari host ke guest (misalnya SSH):
```bash
qemu-system-aarch64 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-net-device,netdev=net0 \
    ...
```
