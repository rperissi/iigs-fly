/*
 * cogslib - shared CoGS client library for the Apple II family.
 *
 * This is the foundation every GS (and later 8-bit) CoGS program sits on:
 * the CoGS Control Panel NDA, the Marinetti link layer, GS-LLM, GS Miner Pro.
 * It owns presence detection, the register I/O, frame send/receive, and a
 * handful of high-level helpers (ping, status, time, wifi, http, json_get).
 *
 * Design mirrors the firmware's portability split (spec v0.6 section 9):
 *   - This protocol layer (cogs.c) is plain C89 with no hardware addresses,
 *     so it compiles under gcc for host unit tests AND under ORCA/C for the
 *     GS, driven by a swappable transport backend.
 *   - The transport backend is a 2-function vtable (read/write one of the 16
 *     DEVSEL registers). Real backend = the slot registers (cogs_io_slot.c,
 *     GS only). Stub backend = an in-memory software model of the card
 *     (cogs_io_stub.c, portable) for development before any hardware or
 *     virtual card exists.
 *
 * Contract: spec/cogs-spec-v0.6.md. Opcodes/errors below mirror it and the
 * firmware's commands.h exactly; when the spec bumps, this header bumps.
 *
 * Portability notes (ORCA/C is C89, int is 16-bit):
 *   - Declarations go at the top of each block; no line (//) comments.
 *   - cogs_u16 is "at least 16 bits"; cogs_u32 is 32 bits (unsigned long).
 */

#ifndef COGS_H
#define COGS_H

typedef unsigned char cogs_u8;
typedef unsigned int  cogs_u16;   /* >= 16 bits (exactly 16 on ORCA/C)   */
typedef unsigned long cogs_u32;   /* 32 bits on both ORCA/C and host     */

/* firmware identity this header is written against (spec v0.6 / commands.h) */
#define COGS_FW_MAJOR   0
#define COGS_PROTO_VER  0x03      /* VERSION register, all v0.x            */

/* ---- register map (spec section 2) ---- */
#define COGS_REG_DATA     0
#define COGS_REG_STATUS   1
#define COGS_REG_RXLO     2
#define COGS_REG_RXHI     3
#define COGS_REG_CONTROL  4
#define COGS_REG_ID       5
#define COGS_REG_VERSION  6

#define COGS_ID_BYTE      0xA2

/* STATUS bits */
#define COGS_ST_RX_AVAIL  0x80
#define COGS_ST_TX_READY  0x40
#define COGS_ST_CMD_BUSY  0x20
#define COGS_ST_LINK_UP   0x10
#define COGS_ST_ERROR     0x08

/* CONTROL bits */
#define COGS_CTL_RESET    0x01
#define COGS_CTL_IRQ_EN   0x02
#define COGS_CTL_FLUSH_RX 0x04
#define COGS_CTL_FLUSH_TX 0x08

/* ---- caps bitmap (PING response, spec section 4) ---- */
#define COGS_CAP_NET      0x01
#define COGS_CAP_TLS      0x02
#define COGS_CAP_HTTP     0x04
#define COGS_CAP_HASH     0x08
#define COGS_CAP_RANDOM   0x10
#define COGS_CAP_JSON     0x20
#define COGS_CAP_NONCE    0x40
#define COGS_CAP_NETTOOLS 0x80   /* RESOLVE + TCP_PING (nslookup / ping) */

/* ---- command opcodes (host -> card) ---- */
#define COGS_OP_PING        0x00
#define COGS_OP_STATUS      0x01
#define COGS_OP_WIFI_SCAN   0x02
#define COGS_OP_WIFI_JOIN   0x03
#define COGS_OP_WIFI_SAVE   0x04
#define COGS_OP_TIME        0x05
#define COGS_OP_PREFS_GET   0x06
#define COGS_OP_PREFS_SET   0x07
#define COGS_OP_NETINFO     0x08
#define COGS_OP_TLS_INFO    0x09
#define COGS_OP_CARD_INFO   0x0A
#define COGS_OP_RESOLVE     0x0B   /* DNS A-lookup (nslookup)               */
#define COGS_OP_TCP_PING    0x0C   /* TCP reachability probe (ping)         */
#define COGS_OP_ICMP_PING   0x0D   /* true ICMP echo ping                   */
#define COGS_OP_TRACEROUTE  0x0E   /* one-hop-per-call traceroute           */

/* Ceiling for the opaque PREFS blob (must match the card's COGS_PREFS_MAX). */
#define COGS_PREFS_MAX      32
#define COGS_OP_CONNECT     0x10
#define COGS_OP_SEND        0x11
#define COGS_OP_CLOSE       0x12
#define COGS_OP_HTTP        0x20
#define COGS_OP_HASH        0x30
#define COGS_OP_RANDOM      0x31
#define COGS_OP_JSON_GET    0x32
#define COGS_OP_NONCE_SCAN  0x33
#define COGS_OP_NONCE_CANCEL 0x34

/* Link-layer carriage for the legacy stacks (Marinetti on GS, IP65 on 8-bit),
 * spec section 12. The card forwards raw frames between the host stack and the
 * radio; inbound frames arrive as COGS_EV_LINK_FRAME ($96) events. Replies
 * follow the universal 0x80|opcode convention (LINK_OPEN -> $C0,
 * LINK_CLOSE -> $C2); see the v0.6 notes erratum. LINK_FRAME is fire-and-forget
 * (no reply). */
#define COGS_OP_LINK_OPEN   0x40
#define COGS_OP_LINK_FRAME  0x41
#define COGS_OP_LINK_CLOSE  0x42
#define COGS_OP_LINK_IP     0x43   /* report GS IPv4 after DHCP/static */

/* Network storage (fw 0.12, spec section 13). Present a disk image at a URL as
 * a 512-byte ProDOS block device: the card resolves + connects (TLS for https)
 * once, then serves blocks via HTTP range reads. VOL_OPEN -> $D0, VOL_READ ->
 * $D1, VOL_CLOSE -> $D2 (all > 0x9F, clear of the event range). A card too old
 * to implement them answers $93 UNSUPPORTED (surfaces as COGS_ERR_PROTO), which
 * is the feature probe. */
#define COGS_OP_VOL_OPEN    0x50
#define COGS_OP_VOL_READ    0x51
#define COGS_OP_VOL_CLOSE   0x52

/* Boot/default volume URL persisted in the card's NV (fw 0.14). The slot ROM
 * mounts handle 0 from this URL with no URL on the wire, so the CDA / SoftAP
 * page set it once and boot-from-card + zero-software mount just work.
 * VOL_URL_SET -> $D3 (payload url_len(1) + url; url_len 0 clears it),
 * VOL_URL_GET -> $D4 (reply url_len(1) + url). Probed by COGS_ERR_PROTO. */
#define COGS_OP_VOL_URL_SET 0x53
#define COGS_OP_VOL_URL_GET 0x54

/* Multi-unit mount table (fw 0.16, FujiNet model). The slot ROM presents
 * COGS_VOL_MAX SmartPort units; unit U (1-based) auto-mounts from NV slot U-1
 * (unit 1 = the boot URL above). The CDA assigns images to units here; changes
 * apply on the next boot.
 * VOL_MOUNT_SET -> $D5 (payload unit(1) + url_len(1) + url; url_len 0 clears),
 * VOL_MOUNT_GET -> $D6 (reply result(1) + unit(1) + url_len(1) + url). */
#define COGS_OP_VOL_MOUNT_SET 0x55
#define COGS_OP_VOL_MOUNT_GET 0x56

/* Catalog/browse base URL persisted in the card's NV (fw 0.25). The SAME slot
 * the ROM config menu's "Image Source" page reads and writes, so the desktop
 * CDA and the boot-ROM browse stay in sync. Empty = the compiled-in default.
 * CAT_URL_SET -> $D9 (payload url_len(1) + url; url_len 0 reverts to default),
 * CAT_URL_GET -> $DA (reply url_len(1) + url). Probed by COGS_ERR_PROTO. */
#define COGS_OP_CAT_URL_SET 0x59
#define COGS_OP_CAT_URL_GET 0x5A

/* Audio catalog base URL persisted in the card's NV (fw 0.93), beside the
 * disk catalog base above. The ROM config Image/Audio Source page and the
 * CDA's Audio page share it. Empty = the compiled-in default
 * (https://a2cogs.com/audio/). Probed by COGS_ERR_PROTO. */
#define COGS_OP_AUD_URL_SET 0x5C
#define COGS_OP_AUD_URL_GET 0x5D

/* Byte streaming (fw 0.93, spec section 4 "Byte streaming"): a pull-model
 * continuous stream for audio (Tool225 PCM) or any raw bytes. The GS pulls at
 * its own pace; the card buffers ahead in a 64 KB ring and stops reading its
 * source socket when the ring is full, so TCP flow control paces the origin.
 * STREAM_OPEN -> $F0 (payload url_len(1) + url; http(s):// = GET body,
 * tcp://host:port = raw socket), STREAM_READ -> $F1 (handle(1) +
 * max_len(u16 LE); reply handle, result, flags, level, len(u16 LE), data),
 * STREAM_CLOSE -> $F2, STREAM_STAT -> $F3. One stream at a time (second OPEN
 * = E_BUSY); old cards answer $93 UNSUPPORTED (COGS_ERR_PROTO), the probe. */
#define COGS_OP_STREAM_OPEN  0x70
#define COGS_OP_STREAM_READ  0x71
#define COGS_OP_STREAM_CLOSE 0x72
#define COGS_OP_STREAM_STAT  0x73

/* STREAM_READ reply flags */
#define COGS_STREAMF_FLOWING 0x01  /* source still open                     */
#define COGS_STREAMF_END     0x02  /* source done and card ring fully drained */
#define COGS_STREAMF_STALLED 0x04  /* ring empty while flowing (origin lag)  */

/* STREAM_STAT states */
#define COGS_STREAM_ST_CLOSED     0
#define COGS_STREAM_ST_CONNECTING 1
#define COGS_STREAM_ST_FLOWING    2
#define COGS_STREAM_ST_ENDED      3
#define COGS_STREAM_ST_ERROR      4

/* Per-READ data cap (the $F1 reply carries 6 header bytes in a 2048-byte
 * frame; mirrors the card's COGS_STREAM_READ_MAX). */
#define COGS_STREAM_READ_MAX 1536

/* A ProDOS block is 512 bytes everywhere in the II world; the card serves only
 * this size. COGS_VOL_MAX mirrors the card's COGS_MAX_VOLS (concurrent mounts =
 * SmartPort units presented); handles run 0..COGS_VOL_MAX-1, units 1..COGS_VOL_MAX.
 * COGS_BOOT_URL_MAX mirrors the card's NV field. */
#define COGS_VOL_BLOCK      512
#define COGS_VOL_MAX        8
#define COGS_BOOT_URL_MAX   255

/* LINK_OPEN mode (spec section 12): bridge = the GS is a real LAN host with its
 * own DHCP lease/IP; NAT = the card does DHCP/NAT and hands up IP packets (the
 * retained fallback). */
#define COGS_LINK_BRIDGE    0
#define COGS_LINK_NAT       1

/* responses are 0x80 | opcode; events 0x90-0x9F (spec section 5) */
#define COGS_RESP(op)       ((cogs_u8)(0x80 | (op)))
#define COGS_EV_CONNECTED   0x90
#define COGS_EV_DATA        0x91
#define COGS_EV_CLOSED      0x92
#define COGS_EV_ERROR       0x93
#define COGS_EV_WIFI_STATE  0x94
#define COGS_EV_HTTP_DONE   0x95
#define COGS_EV_LINK_FRAME  0x96
#define COGS_EV_NONCE_PROGRESS 0x97
#define COGS_EV_SEND_ACK    0x9A   /* v0.6: moved off 0x91 (DATA collision) */

/* ---- error codes (spec section 8) ---- */
#define COGS_E_OK            0
#define COGS_E_NOT_JOINED    1
#define COGS_E_DNS_FAIL      2
#define COGS_E_CONN_TIMEOUT  3
#define COGS_E_CONN_REFUSED  4
#define COGS_E_TLS_HANDSHAKE 5
#define COGS_E_CERT_VERIFY   6
#define COGS_E_NO_FREE_SOCK  7
#define COGS_E_SOCKET_STATE  8
#define COGS_E_BUF_OVERRUN   9
#define COGS_E_BAD_FRAME    10
#define COGS_E_UNSUPPORTED  11
#define COGS_E_INTERNAL     12
#define COGS_E_NO_RETAINED  13
#define COGS_E_JSON_NOPATH  14
#define COGS_E_BUSY         15
#define COGS_E_VOL_NOMOUNT  16   /* VOL op on a handle with no open volume   */
#define COGS_E_VOL_RANGE    17   /* VOL_READ block past the volume's end     */
#define COGS_E_STREAM_NONE  18   /* STREAM op with no open stream (fw 0.93)  */

/* library-local status codes (negative; distinct from card error codes) */
#define COGS_OK              0
#define COGS_ERR_TIMEOUT    (-1)   /* a STATUS poll spun past its cap     */
#define COGS_ERR_NO_CARD    (-2)   /* presence probe failed               */
#define COGS_ERR_OVERSIZE   (-3)   /* a frame did not fit the caller buf  */
#define COGS_ERR_PROTO      (-4)   /* unexpected opcode / malformed reply */

/* cogs_poll_event-only: a positive (non-error, non-OK) sentinel meaning the
 * card had no frame waiting on this non-blocking check. The link layer's idle
 * pump hits this constantly, so it must NOT be confused with COGS_OK (a frame
 * was read) or a negative transport error. */
#define COGS_NO_EVENT        1

/* connect flags (spec section 4) */
#define COGS_CONNECT_TLS    0x01
#define COGS_CONNECT_NOVERIFY 0x02

/* http flags (spec section 4) */
#define COGS_HTTP_STREAM    0x01
#define COGS_HTTP_HDRS      0x02
#define COGS_HTTP_RETAIN    0x04
#define COGS_HTTP_KEEPALIVE 0x08   /* fw 0.29+: park the connection after the
                                    * response so the next request to the same
                                    * host rides it (no fresh TLS handshake).
                                    * Ignored by older firmware. */

/* ---- transport backend: read/write one of the 16 DEVSEL registers ----
 * rd returns int, not cogs_u8: ORCA/C miscompiles a char-returning function
 * called through a function pointer (the byte comes back in the wrong place),
 * which silently broke STATUS polling on the real 65816. int is the natural
 * return width and is robust on both ORCA/C and host gcc. The value is a
 * register byte (0..255); callers mask/assign into cogs_u8 as needed. */
typedef struct cogs_io {
    int  (*rd)(struct cogs_io *io, int reg);
    void (*wr)(struct cogs_io *io, int reg, cogs_u8 v);
    /* Optional burst read (2026-07): pop n bytes from DATA into dst. The
     * caller must already know n bytes are queued (RXLO/RXHI). Lets a
     * backend hoist its per-register address arithmetic out of the loop -
     * the slot backend recomputes a 32-bit register address per rd() call,
     * which at ORCA/C -O0 dominated large payload drains (CDA catalog
     * pages). Set by cogs_io_slot_init / cogs_io_stub_init; a backend that
     * leaves it NULL just gets the per-byte rd() path. */
    void (*rdn)(struct cogs_io *io, cogs_u8 *dst, cogs_u16 n);
    void *priv;
} cogs_io;

/* ---- a CoGS session: a bound transport plus a polling spin cap ---- */
typedef struct {
    cogs_io *io;
    cogs_u32 spin_cap;   /* max STATUS polls before COGS_ERR_TIMEOUT     */
} cogs;

/* Decoded PING reply (spec section 4). */
typedef struct {
    cogs_u8 fw_major;
    cogs_u8 fw_minor;
    cogs_u8 proto_ver;
    cogs_u8 caps;
} cogs_ping_reply;

/* Decoded STATUS reply (spec section 4). temp_c10 is the on-chip die
 * temperature in tenths of a degree Celsius (e.g. 423 = 42.3 C), appended by
 * fw 0.7+. has_temp is 0 on older firmware (or the emulator) that omits it -
 * the 9-byte prefix is identical either way. fw 0.8+ extends the tail again
 * with the live vitals (VSYS millivolts + uptime seconds); has_vitals is 0 when
 * the card stops at the 0.7 (11-byte) form. */
typedef struct {
    cogs_u8  link_state;       /* 0 down, 1 joining, 2 up + IP           */
    cogs_u8  ip[4];
    signed char rssi;
    cogs_u16 free_heap_kb;
    cogs_u8  active_sockets;
    cogs_u8  has_temp;         /* 1 if temp_c10 is present (fw 0.7+)     */
    int      temp_c10;         /* die temp, tenths of C; valid iff has_temp */
    cogs_u8  has_vitals;       /* 1 if vsys_mv/uptime_s present (fw 0.8+) */
    cogs_u16 vsys_mv;          /* supply rail mV (slot +5V); 0 = no sensor */
    cogs_u32 uptime_s;         /* seconds since card boot                 */
} cogs_status_reply;

/* Decoded CARD_INFO reply ($8A, fw 0.8+): the card's static identity. serial
 * is the raw 8-byte unique board id; serial_hex is the same as a NUL-terminated
 * uppercase hex string for display. reset_reason: 0 clean power/reset, 1
 * watchdog. clock_hz is the running clk_sys. */
typedef struct {
    cogs_u8  serial[8];
    char     serial_hex[17];   /* 16 hex chars + NUL                      */
    cogs_u8  reset_reason;     /* 0 power/reset, 1 watchdog               */
    cogs_u32 clock_hz;
} cogs_cardinfo_reply;

/* Decoded NETINFO reply (spec section 4, $88): the diagnostic superset of
 * STATUS. The first fields mirror STATUS; the rest are the extended details a
 * card may report. dns2 / fields the card cannot source come back as 0.0.0.0;
 * ssid is NUL-terminated (ssid_len is the length, 0 = not associated / name not
 * reported). ll_active/ll_mode are the trailer after the SSID (fw 0.60+); older
 * cards omit them and decode as closed. */
typedef struct {
    cogs_u8  link_state;       /* 0 down, 1 joining, 2 up + IP           */
    cogs_u8  ip[4];
    cogs_u8  subnet[4];
    cogs_u8  gateway[4];
    cogs_u8  dns1[4];
    cogs_u8  dns2[4];
    cogs_u8  mac[6];
    signed char rssi;
    cogs_u8  ssid_len;         /* 0..32                                  */
    char     ssid[33];         /* NUL-terminated connected network name  */
    cogs_u8  ll_active;        /* 1 while LINK_OPEN is held               */
    cogs_u8  ll_mode;          /* COGS_LINK_BRIDGE / COGS_LINK_NAT        */
    cogs_u8  gs_ip[4];         /* Marinetti GS address (0 = unknown)      */
} cogs_netinfo_reply;

/* Decoded TLS_INFO reply ($89): the most recent TLS session the CARD itself
 * terminated - proof it did real TLS, not a proxy. valid=0 if no TLS handshake
 * has happened since boot; verified=1 if the server cert chain validated. All
 * strings are NUL-terminated (empty if the card didn't report them). */
typedef struct {
    cogs_u8 valid;             /* 1 once a TLS session has been captured     */
    cogs_u8 verified;          /* 1 if the peer cert chain verified OK        */
    char    version[16];       /* "TLSv1.2"                                   */
    char    cipher[48];        /* "TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256"     */
    char    cn[48];            /* peer cert subject CN, "example.com"         */
} cogs_tls_reply;

/* One network from a WIFI_SCAN ($82). ssid is NUL-terminated. auth: 0 open,
 * 1 secured (any WPA/WEP). rssi in dBm (negative; closer to 0 = stronger). */
#define COGS_SCAN_MAX 24
typedef struct {
    signed char rssi;
    cogs_u8     auth;
    cogs_u8     ssid_len;       /* 1..32                                  */
    char        ssid[33];
} cogs_scan_net;

/* Decoded WIFI_SCAN reply: up to COGS_SCAN_MAX networks, strongest first. */
typedef struct {
    cogs_u8       count;
    cogs_scan_net nets[COGS_SCAN_MAX];
} cogs_scan_reply;

/* Decoded RESOLVE reply ($8B): a DNS A-lookup (nslookup). result is the card's
 * verdict: COGS_E_OK with count >= 1, or COGS_E_DNS_FAIL / COGS_E_NOT_JOINED
 * with count 0. Each addr[i] is dotted-quad bytes a.b.c.d. */
#define COGS_RESOLVE_MAX 4
typedef struct {
    cogs_u8 result;
    cogs_u8 count;                     /* 0..COGS_RESOLVE_MAX               */
    cogs_u8 addr[COGS_RESOLVE_MAX][4];
} cogs_resolve_reply;

/* Decoded TCP_PING reply ($8C): a TCP reachability "ping". result COGS_E_OK
 * means the connect succeeded - rtt_ms is the round-trip and addr the resolved
 * IP. Other results (COGS_E_DNS_FAIL / CONN_TIMEOUT / CONN_REFUSED /
 * NOT_JOINED) leave rtt_ms 0 (addr holds the resolved IP when DNS got that
 * far, else 0.0.0.0). */
typedef struct {
    cogs_u8  result;
    cogs_u16 rtt_ms;
    cogs_u8  addr[4];
} cogs_probe_reply;

/* Decoded TRACEROUTE reply ($8E): one hop. result COGS_E_OK means the probe ran
 * (a hop answered OR the hop timed out); a setup failure (NOT_JOINED / DNS_FAIL /
 * INTERNAL) leaves the rest zero. ttl echoes the requested hop. addr is the
 * responding hop (0.0.0.0 when timed out). flags: bit0 DONE = this is the final
 * destination (stop walking), bit1 TIMEOUT = no reply at this hop (print "*"). */
#define COGS_TR_DONE    0x01
#define COGS_TR_TIMEOUT 0x02
typedef struct {
    cogs_u8  result;
    cogs_u8  ttl;
    cogs_u8  addr[4];
    cogs_u16 rtt_ms;
    cogs_u8  flags;
} cogs_trace_reply;

/* Decoded HTTP result (spec sections 4-5). One cogs_http_* call drives the
 * whole DNS + TCP + (TLS) + request + response on the card and demuxes the
 * DATA / HTTP_DONE / ERROR event stream into this. */
typedef struct {
    cogs_u16 status;     /* HTTP status from $95 HTTP_DONE (0 if none)        */
    cogs_u32 total;      /* total decoded body bytes the card delivered       */
    cogs_u16 body_len;   /* bytes actually copied into the caller buffer      */
    cogs_u8  truncated;  /* 1 if total > caller cap (body_out is partial)     */
    cogs_u8  err;        /* card error code (COGS_E_*); COGS_E_OK on success  */
} cogs_http_reply;

/* Decoded JSON_GET result (spec section 6). The card walks the retained HTTP
 * response along a dot/bracket path and streams the one value back as $B2
 * chunk frames; cogs_json_get concatenates them into this. */
typedef struct {
    cogs_u16 value_len;  /* total decoded value length the card reported      */
    cogs_u16 copied;     /* value bytes actually copied into the caller buf   */
    cogs_u8  truncated;  /* 1 if value_len > caller cap (out is partial)      */
    cogs_u8  err;        /* card error code (COGS_E_*); COGS_E_OK on success  */
} cogs_json_reply;

/* Decoded VOL_OPEN reply ($D0): the card mounted (or rejected) a URL as a block
 * device. result is the card's verdict (COGS_E_OK on success); total_blocks is
 * the volume size in 512-byte ProDOS blocks (0 on any error). handle echoes the
 * mount slot the open used. */
typedef struct {
    cogs_u8  handle;
    cogs_u8  result;          /* COGS_E_*                                   */
    cogs_u32 total_blocks;    /* 512-byte blocks; 0 on error                */
} cogs_vol_reply;

/* ---- lifecycle ---- */
void cogs_init(cogs *c, cogs_io *io);
void cogs_reset(cogs *c);

/* ---- low-level framing ----
 * cogs_send_frame: poll TX_READY and push [opcode][len lo][len hi][payload].
 * cogs_recv_frame: poll RX_AVAIL and pull one frame; writes *opcode and
 *   copies up to cap payload bytes, setting *len to the true payload length
 *   (COGS_ERR_OVERSIZE if it exceeds cap). Returns COGS_OK or a COGS_ERR_*. */
int cogs_send_frame(cogs *c, cogs_u8 opcode,
                    const cogs_u8 *payload, cogs_u16 len);
int cogs_recv_frame(cogs *c, cogs_u8 *opcode,
                    cogs_u8 *payload, cogs_u16 cap, cogs_u16 *len);

/* ---- presence ----
 * cogs_present: read ID/VERSION/STATUS on the bound transport; 1 if a card
 *   answers (ID == $A2, VERSION != 0, STATUS != $FF). */
int cogs_present(cogs *c);

/* ---- high-level helpers (each is one send + one recv round trip) ---- */
int cogs_ping(cogs *c, cogs_ping_reply *out);
int cogs_get_status(cogs *c, cogs_status_reply *out);
/* NETINFO ($08): the STATUS superset (connected SSID + subnet/gateway/DNS/MAC).
 * Returns COGS_ERR_PROTO if the card is too old to implement it (it answers
 * $93 ERROR/UNSUPPORTED), so callers can fall back to cogs_get_status(). */
int cogs_netinfo(cogs *c, cogs_netinfo_reply *out);

/* TLS_INFO ($09 -> $89): fetch details of the card's most recent TLS session
 * (version, cipher, server cert CN, verify result). Returns COGS_OK with
 * out->valid==0 if no TLS session has happened yet, or COGS_ERR_PROTO if the
 * firmware predates the command (answered UNSUPPORTED). */
int cogs_tls_info(cogs *c, cogs_tls_reply *out);
/* Read the card's static identity (serial / reset reason / clock). Returns
 * COGS_ERR_PROTO on pre-0.8 firmware that answers CARD_INFO ($0A) UNSUPPORTED. */
int cogs_card_info(cogs *c, cogs_cardinfo_reply *out);
int cogs_time(cogs *c, cogs_u32 *unix_out);
int cogs_wifi_join(cogs *c, const char *ssid, const char *psk, cogs_u8 *result);
int cogs_wifi_save(cogs *c, const char *ssid, const char *psk, cogs_u8 *result);

/* WIFI_SCAN ($02): scan for networks and collect the $82 list into out (best
 * signal first). The card's radio scan takes a couple seconds, so this blocks
 * longer than other helpers (it widens the status spin cap for the one wait).
 * Returns COGS_OK with out->count populated, or COGS_ERR_PROTO if the card
 * answered ERROR/UNSUPPORTED. */
int cogs_wifi_scan(cogs *c, cogs_scan_reply *out);

/* Basic network tools (need CAP_NETTOOLS; all feature-test cleanly - a card
 * that predates them answers $93 UNSUPPORTED, which surfaces as COGS_ERR_PROTO).
 *   cogs_resolve ($0B -> $8B): DNS A-lookup of host (nslookup). Fills out with
 *     up to COGS_RESOLVE_MAX addresses; check out->result for the DNS verdict.
 *   cogs_probe ($0C -> $8C): time a TCP connect to host:port (a reachability
 *     "ping"). Fills out with result, round-trip ms, and the resolved IP.
 *   cogs_icmp_ping ($0D -> $8D): a true ICMP echo ping (the "real" ping, no
 *     port). Same reply shape as cogs_probe; result COGS_E_OK means the echo
 *     reply came back. Folded under CAP_NETTOOLS but probed by call: a card
 *     without ICMP returns COGS_ERR_PROTO, so callers can fall back to
 *     cogs_probe. ICMP and the TCP probe are complementary - some hosts answer
 *     one but not the other.
 * All return COGS_OK on a completed round trip (then inspect out->result), or
 * a negative COGS_ERR_* on a transport failure. */
int cogs_resolve(cogs *c, const char *host, cogs_resolve_reply *out);
int cogs_probe(cogs *c, const char *host, cogs_u16 port, cogs_probe_reply *out);
int cogs_icmp_ping(cogs *c, const char *host, cogs_probe_reply *out);

/* cogs_traceroute ($0E -> $8E): probe ONE hop (the given ttl) toward host and
 * fill out with the hop that answered. The caller drives the trace by looping ttl
 * = 1, 2, 3, ... and stopping when out->flags has COGS_TR_DONE (reached the
 * target), out->result is a setup failure, or a hop cap is hit. Also folded under
 * CAP_NETTOOLS and probed by call - a card without it returns COGS_ERR_PROTO.
 * Returns COGS_OK on a completed round trip (then inspect out), else a negative
 * COGS_ERR_*. */
int cogs_traceroute(cogs *c, const char *host, cogs_u8 ttl, cogs_trace_reply *out);

/* PREFS_GET ($06) / PREFS_SET ($07): read/write the small opaque settings blob
 * the GS owns and the card persists in NV. The card never interprets it.
 *   cogs_prefs_get: up to 'cap' bytes into out, *len = bytes returned (0 when
 *     nothing stored). *result is the card's E_* (E_UNSUPPORTED if the firmware
 *     can't persist yet - treat as "no saved prefs").
 *   cogs_prefs_set: store 'len' (<= COGS_PREFS_MAX) bytes; *result is the E_*. */
int cogs_prefs_get(cogs *c, cogs_u8 *out, cogs_u16 cap, cogs_u16 *len,
                   cogs_u8 *result);
int cogs_prefs_set(cogs *c, const cogs_u8 *data, cogs_u8 len, cogs_u8 *result);

/* ---- HTTP (the convenience path: one command does DNS+TCP+TLS+request) ----
 * The card streams the response back as events; these helpers run that event
 * loop and fill *out. Return COGS_OK when the transaction reached a terminal
 * frame - check out->err (COGS_E_OK == success) and out->status. A negative
 * COGS_ERR_* return is a transport-level failure (timed out, malformed).
 *
 * Requires the link up (cogs_wifi_join first). 'sock' is a socket index
 * (0..N-1). Up to 'cap' decoded body bytes are copied to body_out; out->total
 * is the full length and out->truncated flags if it did not all fit.
 *
 * cogs_http_request: method ("GET"/"POST"/...), absolute url ("http://host/..."),
 *   optional extra hdrs (CRLF/LF separated, NUL-terminated, or NULL), optional
 *   request body. flags are COGS_HTTP_* (0 for a plain buffered GET). The card
 *   always injects Host/Connection/User-Agent/Content-Length itself.
 * cogs_http_get: GET with no extra headers or body. */
int cogs_http_request(cogs *c, cogs_u8 sock, const char *method,
                      const char *url, const char *hdrs,
                      const cogs_u8 *body_in, cogs_u16 body_in_len,
                      cogs_u8 flags,
                      cogs_u8 *body_out, cogs_u16 cap, cogs_http_reply *out);
int cogs_http_get(cogs *c, cogs_u8 sock, const char *url,
                  cogs_u8 *body_out, cogs_u16 cap, cogs_http_reply *out);

/* Split request (2026-07, for UI prefetch): cogs_http_begin sends the HTTP
 * command frame and returns immediately - the card fetches while the GS does
 * something else (waits for a key, renders). cogs_http_finish then runs the
 * normal collect loop for that socket, draining whatever DATA events have
 * already queued in the card's RX ring plus the remainder as it arrives.
 *
 * IMPORTANT contract while a begin is outstanding: do NOT make any other
 * cogslib call that receives frames (any command round trip). recv_expected
 * discards frames it is not looking for, and it would eat the in-flight
 * response. Call cogs_http_finish first, then do the other work.
 * cogs_http_request is exactly begin + finish back to back. */
int cogs_http_begin(cogs *c, cogs_u8 sock, const char *method,
                    const char *url, const char *hdrs,
                    const cogs_u8 *body_in, cogs_u16 body_in_len,
                    cogs_u8 flags);
int cogs_http_finish(cogs *c, cogs_u8 sock,
                     cogs_u8 *body_out, cogs_u16 cap, cogs_http_reply *out);

/* ---- JSON_GET (extract one field from the card's retained response) -------
 * Precondition: a prior cogs_http_request on this 'sock' with COGS_HTTP_RETAIN
 * set, so the card buffered the response body instead of streaming it. This
 * then asks the card to walk that document along 'path' (a dot/bracket subset:
 * "key", "a.b.c", "arr[0]", "a[2].b.c", or "" for the whole root) and return
 * the value - the GS never parses JSON itself.
 *
 * Returns COGS_OK once a terminal reply arrived; check jr->err (COGS_E_OK on
 * success, COGS_E_NO_RETAINED / COGS_E_JSON_NOPATH on the common failures).
 * Up to 'cap' value bytes are copied to 'out'; jr->value_len is the full
 * length and jr->truncated flags if it did not all fit. A negative COGS_ERR_*
 * return is a transport-level failure. */
int cogs_json_get(cogs *c, cogs_u8 sock, const char *path,
                  cogs_u8 *out, cogs_u16 cap, cogs_json_reply *jr);

/* ---- link layer (spec section 12) ---------------------------------------
 * The carriage path a GS-side Marinetti link layer (or an 8-bit IP65 driver)
 * sits on: the host TCP/IP stack builds whole frames, hands them down here,
 * and the card forwards them to/from the radio. Unlike the smart-socket path
 * (CONNECT/HTTP/JSON_GET), the card does no protocol work - it is a NIC.
 *
 * cogs_link_open: switch the card into link-layer mode. mode is COGS_LINK_*.
 *   *result is the card's E_* ($C0 payload): E_OK entered, E_UNSUPPORTED on a
 *   card too old to bridge. Call once at "connect"; pair with cogs_link_close.
 * cogs_link_frame: hand one outbound frame to the card. Fire-and-forget - no
 *   reply (the spec drops the per-frame ack so a busy stack is not throttled
 *   by a round trip per packet). Returns a transport COGS_ERR_* only if the
 *   bytes could not be pushed.
 * cogs_link_close: leave link-layer mode. *result is the card's E_* ($C2).
 * cogs_link_ip: report the GS IPv4 after DHCP/static ($43 -> $C3). */
int cogs_link_open(cogs *c, cogs_u8 mode, cogs_u8 *result);
int cogs_link_frame(cogs *c, const cogs_u8 *frame, cogs_u16 len);
int cogs_link_close(cogs *c, cogs_u8 *result);
int cogs_link_ip(cogs *c, const cogs_u8 ip[4], cogs_u8 *result);

/* ---- network storage (spec section 13) ----------------------------------
 * The block path a GS-side mount sits on: the card turns a disk image at a URL
 * into 512-byte ProDOS blocks it serves over HTTP range reads. These three are
 * the host half of a "network CFFA" - a GS app (or, later, a SmartPort/GS-OS
 * block driver) calls vol_open once, vol_read per block, vol_close at unmount.
 *
 * Like HTTP, the card does the open/read asynchronously (DNS + TCP + TLS + the
 * range fetch happen on the card, then it emits the $D0/$D1 reply), so these
 * helpers widen the spin cap while they wait. A card too old to implement VOL
 * answers $93 UNSUPPORTED, surfaced as COGS_ERR_PROTO (the feature probe).
 *
 * cogs_vol_open ($50 -> $D0): mount url ("http://host/img.2mg" or https://) at
 *   handle (0..COGS_VOL_MAX-1). flags reserved, pass 0 (read-only). Fills out
 *   with the card's result + total_blocks. Returns COGS_OK once the $D0 landed
 *   (then check out->result), or a negative COGS_ERR_* transport failure. The
 *   card reads the image's 2IMG header (or sizes a raw image from Content-Range)
 *   to get the block count - the GS never parses the container.
 * cogs_vol_read ($51 -> $D1): read one 512-byte block into out (must hold >=
 *   COGS_VOL_BLOCK bytes). *result is the card's E_* (COGS_E_OK with the data,
 *   COGS_E_VOL_RANGE past the end, COGS_E_VOL_NOMOUNT on a closed handle). On
 *   any non-OK result the out buffer is left untouched. Returns COGS_OK once the
 *   $D1 landed, else a negative COGS_ERR_*.
 * cogs_vol_close ($52 -> $D2): unmount and free the card's kept-alive socket +
 *   cache. *result is the card's E_*. Synchronous on the card. */
int cogs_vol_open(cogs *c, cogs_u8 handle, cogs_u8 flags, const char *url,
                  cogs_vol_reply *out);
int cogs_vol_read(cogs *c, cogs_u8 handle, cogs_u32 block,
                  cogs_u8 *out, cogs_u8 *result);
int cogs_vol_close(cogs *c, cogs_u8 handle, cogs_u8 *result);

/* Boot/default volume URL in the card's NV (fw 0.14, the slot ROM mounts it on
 * handle 0). Set it once and boot-from-card / zero-software mount survive power
 * cycles, exactly like the card's flash pref on iron.
 *
 * cogs_vol_url_set ($53 -> $D3): persist 'url' (a NUL-terminated string up to
 *   COGS_BOOT_URL_MAX bytes; pass NULL or "" to clear it). *result is the card's
 *   E_* (COGS_E_OK stored, COGS_E_UNSUPPORTED if the firmware can't persist it).
 *   Returns COGS_ERR_OVERSIZE if the URL is too long, else COGS_OK once $D3
 *   landed, else a negative transport COGS_ERR_*.
 * cogs_vol_url_get ($54 -> $D4): copy the stored URL into out[cap] (always
 *   NUL-terminated when cap > 0); *len gets the byte count. A card too old to
 *   store it answers an error frame; that is reported as *result =
 *   COGS_E_UNSUPPORTED with *len = 0 and COGS_OK (not a transport failure). */
int cogs_vol_url_set(cogs *c, const char *url, cogs_u8 *result);
int cogs_vol_url_get(cogs *c, char *out, cogs_u16 cap, cogs_u16 *len,
                     cogs_u8 *result);

/* Per-unit mount table (fw 0.16, multi-unit SmartPort). The CDA uses these to
 * assign network images to SmartPort units; changes take effect on the next
 * boot. 'unit' is 1-based (1..COGS_VOL_MAX); unit 1 is the boot volume and is
 * the same NV slot cogs_vol_url_set/get touch.
 *
 * cogs_vol_mount_set ($55 -> $D5): persist 'url' for 'unit' (NUL-terminated, up
 *   to COGS_BOOT_URL_MAX bytes; NULL or "" clears the slot). *result is the
 *   card's E_*. Returns COGS_ERR_OVERSIZE if too long, COGS_OK once $D5 landed.
 * cogs_vol_mount_get ($56 -> $D6): copy 'unit's stored URL into out[cap]
 *   (always NUL-terminated when cap > 0); *len gets the byte count. An old card
 *   reports *result = COGS_E_UNSUPPORTED, *len = 0, COGS_OK. */
int cogs_vol_mount_set(cogs *c, cogs_u8 unit, const char *url, cogs_u8 *result);
int cogs_vol_mount_get(cogs *c, cogs_u8 unit, char *out, cogs_u16 cap,
                       cogs_u16 *len, cogs_u8 *result);

/* Catalog/browse base URL (fw 0.25) - the shared "Image Source" the ROM config
 * menu and the desktop CDA both use, so configuring it in one place covers both.
 *
 * cogs_cat_url_set ($59 -> $D9): persist 'url' (NUL-terminated, up to
 *   COGS_BOOT_URL_MAX bytes; NULL or "" reverts to the compiled-in default).
 *   *result is the card's E_*. Returns COGS_ERR_OVERSIZE if too long, else
 *   COGS_OK once $D9 landed.
 * cogs_cat_url_get ($5A -> $DA): copy the stored base into out[cap] (always
 *   NUL-terminated when cap > 0); *len gets the byte count. A card too old to
 *   store it reports *result = COGS_E_UNSUPPORTED, *len = 0, COGS_OK. */
int cogs_cat_url_set(cogs *c, const char *url, cogs_u8 *result);
int cogs_cat_url_get(cogs *c, char *out, cogs_u16 cap, cogs_u16 *len,
                     cogs_u8 *result);

/* Audio catalog base URL (fw 0.93) - the "Audio Source" half of the ROM
 * config Image/Audio Source page, shared with the CDA's Audio page. Same
 * contract as the cat_url pair above (empty reverts to the compiled default;
 * old card reports COGS_E_UNSUPPORTED + empty, not a transport failure). */
int cogs_aud_url_set(cogs *c, const char *url, cogs_u8 *result);
int cogs_aud_url_get(cogs *c, char *out, cogs_u16 cap, cogs_u16 *len,
                     cogs_u8 *result);

/* ---- byte streaming (fw 0.93) --------------------------------------------
 * Pull-model continuous stream, built for audio: the caller drains the card's
 * 64 KB ring at its own pace and the card paces the origin via TCP flow
 * control. All calls are synchronous round trips on the FIFO.
 *
 * cogs_stream_open ($70 -> $F0): open 'url' (NUL-terminated; http(s):// = the
 *   GET body, tcp://host:port = raw socket bytes). *result is the card's E_*
 *   (COGS_E_BUSY = a stream is already open; a card without the feature
 *   reports COGS_E_UNSUPPORTED, the probe). The card connects during this
 *   call; expect it to take a beat.
 * cogs_stream_read ($71 -> $F1): copy up to maxlen (capped at
 *   COGS_STREAM_READ_MAX) ring bytes into out. Never blocks: *len may be 0
 *   while the card refills. *flags = COGS_STREAMF_* bits (END = source done
 *   and ring drained); *level = ring fill 0-255, the "how far ahead is the
 *   card" gauge.
 * cogs_stream_close ($72 -> $F2): close the source, free the ring.
 * cogs_stream_stat ($73 -> $F3): diag: *state = COGS_STREAM_ST_*, *level as
 *   above, *buffered = ring bytes, *total = bytes delivered since open. */
int cogs_stream_open(cogs *c, const char *url, cogs_u8 *result);
int cogs_stream_read(cogs *c, cogs_u8 *out, cogs_u16 maxlen, cogs_u16 *len,
                     cogs_u8 *flags, cogs_u8 *level, cogs_u8 *result);
int cogs_stream_close(cogs *c, cogs_u8 *result);
int cogs_stream_stat(cogs *c, cogs_u8 *state, cogs_u8 *level,
                     cogs_u32 *buffered, cogs_u32 *total, cogs_u8 *result);

/* ---- non-blocking event pump --------------------------------------------
 * Read at most one pending card->host frame WITHOUT spinning: it checks
 * STATUS once and, only if RX_AVAIL is set, pulls the frame. This is the
 * idle-poll the link layer calls on every Marinetti timer tick to drain
 * inbound COGS_EV_LINK_FRAME ($96) events (and any other unsolicited event)
 * without blocking the GS.
 *
 * Returns COGS_NO_EVENT (positive) when nothing was waiting - the common case
 * on an idle link, and NOT an error. On a frame: writes *opcode and *len and
 * copies up to cap payload bytes, returning COGS_OK (or COGS_ERR_OVERSIZE if
 * the payload exceeded cap, in which case *len is still the true length and
 * the frame is fully drained). A negative COGS_ERR_* is a transport failure. */
int cogs_poll_event(cogs *c, cogs_u8 *opcode, cogs_u8 *payload,
                    cogs_u16 cap, cogs_u16 *len);

#endif
