/*
 * cogslib protocol layer - portable C89 (gcc for host tests, ORCA/C for GS).
 * No hardware addresses live here; all register access goes through the
 * cogs_io vtable. See cogs.h for the contract.
 */

#include "cogs.h"

#define COGS_DEFAULT_SPIN 200000L
/* STATUS polls allowed while waiting for async net-tool replies ($8B/$8C).
 * Literal (not 200000*40): ORCA/C pulls in ~MUL4 for a runtime multiply even
 * when the CDA unity-builds cogs.c at -O0. */
#define COGS_ASYNC_SPIN 8000000L

void cogs_init(cogs *c, cogs_io *io)
{
    c->io = io;
    c->spin_cap = COGS_DEFAULT_SPIN;
}

void cogs_reset(cogs *c)
{
    c->io->wr(c->io, COGS_REG_CONTROL, COGS_CTL_RESET);
}

/* Spin on STATUS until (status & bit) is set, or the cap is hit.
 *
 * The register read is pulled into a local 's' before the test instead of
 * sitting inline in the loop condition. ORCA/C at -O255 miscompiles the
 * inline form (the poll never matches and always times out); reading into a
 * local each iteration matches the known-good pattern and prevents the
 * optimizer from hoisting/mangling the function-pointer read out of the loop.
 * bit is int (not cogs_u8) for the same toolchain robustness reason. */
static int wait_status(cogs *c, int bit)
{
    cogs_u32 i;
    cogs_io *io = c->io;
    int      s;

    for (i = 0; i < c->spin_cap; i++) {
        s = io->rd(io, COGS_REG_STATUS);
        if (s & bit) {
            return COGS_OK;
        }
    }
    return COGS_ERR_TIMEOUT;
}

int cogs_send_frame(cogs *c, cogs_u8 opcode,
                    const cogs_u8 *payload, cogs_u16 len)
{
    cogs_io *io = c->io;
    cogs_u16 i;
    cogs_u8  hdr[3];

    hdr[0] = opcode;
    hdr[1] = (cogs_u8)(len & 0xFF);
    hdr[2] = (cogs_u8)((len >> 8) & 0xFF);

    for (i = 0; i < 3; i++) {
        if (wait_status(c, COGS_ST_TX_READY) != COGS_OK) {
            return COGS_ERR_TIMEOUT;
        }
        io->wr(io, COGS_REG_DATA, hdr[i]);
    }
    for (i = 0; i < len; i++) {
        if (wait_status(c, COGS_ST_TX_READY) != COGS_OK) {
            return COGS_ERR_TIMEOUT;
        }
        io->wr(io, COGS_REG_DATA, payload[i]);
    }
    return COGS_OK;
}

static int drain_bytes(cogs *c, cogs_u8 *dst, cogs_u16 cap, cogs_u16 n);

int cogs_recv_frame(cogs *c, cogs_u8 *opcode,
                    cogs_u8 *payload, cogs_u16 cap, cogs_u16 *len)
{
    cogs_io *io = c->io;
    cogs_u8  lo, hi;
    cogs_u16 n;

    if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
        return COGS_ERR_TIMEOUT;
    }
    *opcode = io->rd(io, COGS_REG_DATA);

    if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
        return COGS_ERR_TIMEOUT;
    }
    lo = io->rd(io, COGS_REG_DATA);
    if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
        return COGS_ERR_TIMEOUT;
    }
    hi = io->rd(io, COGS_REG_DATA);
    n = (cogs_u16)(((cogs_u16)hi << 8) | lo);
    *len = n;

    /* Payload drain, count-gated (2026-07): the old loop paid a full
     * wait_status() call (function call + loop setup + a STATUS read) for
     * EVERY payload byte, which on a 2.8 MHz GS at the CDA's -O0 build made
     * a 4-8 KB catalog page take seconds to cross the slot - the card was
     * long done while the GS was still popping bytes. RXLO/RXHI (spec
     * section 2, present since the first firmware) report exactly how many
     * bytes are queued, and reading RXLO latches RXHI atomically on the
     * card, so we read the count once and then pop DATA in a tight loop
     * with no per-byte poll (see drain_bytes). */
    return drain_bytes(c, payload, cap, n);
}

int cogs_present(cogs *c)
{
    cogs_io *io = c->io;
    cogs_u8  id, ver, st;

    id  = io->rd(io, COGS_REG_ID);
    ver = io->rd(io, COGS_REG_VERSION);
    st  = io->rd(io, COGS_REG_STATUS);
    return (id == COGS_ID_BYTE && ver != 0 && st != 0xFF) ? 1 : 0;
}

/* Receive frames until the expected response opcode shows up, discarding
 * any unsolicited events that arrive first. Bounded by a small skip cap so
 * a never-arriving reply still times out. */
static int recv_expected(cogs *c, cogs_u8 want,
                         cogs_u8 *payload, cogs_u16 cap, cogs_u16 *len)
{
    int      rc;
    int      skips;
    cogs_u8  op;

    for (skips = 0; skips < 16; skips++) {
        rc = cogs_recv_frame(c, &op, payload, cap, len);
        if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
            return rc;
        }
        if (op == want) {
            return rc;
        }
        if (op == COGS_EV_ERROR) {
            return COGS_ERR_PROTO;
        }
        /* else an unrelated event; loop and read the next frame */
    }
    return COGS_ERR_PROTO;
}

int cogs_ping(cogs *c, cogs_ping_reply *out)
{
    cogs_u8  buf[8];
    cogs_u16 n;
    int      rc;

    rc = cogs_send_frame(c, COGS_OP_PING, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_PING), buf, sizeof(buf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 4) {
        return COGS_ERR_PROTO;
    }
    out->fw_major  = buf[0];
    out->fw_minor  = buf[1];
    out->proto_ver = buf[2];
    out->caps      = buf[3];
    return COGS_OK;
}

int cogs_get_status(cogs *c, cogs_status_reply *out)
{
    cogs_u8  buf[24];
    cogs_u16 n;
    int      rc;

    rc = cogs_send_frame(c, COGS_OP_STATUS, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_STATUS), buf, sizeof(buf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 9) {
        return COGS_ERR_PROTO;
    }
    out->link_state    = buf[0];
    out->ip[0]         = buf[1];
    out->ip[1]         = buf[2];
    out->ip[2]         = buf[3];
    out->ip[3]         = buf[4];
    out->rssi          = (signed char)buf[5];
    out->free_heap_kb  = (cogs_u16)(((cogs_u16)buf[7] << 8) | buf[6]);
    out->active_sockets = buf[8];
    /* fw 0.7+ appends the die temperature (signed LE16, tenths of C). Older
     * firmware sends only 9 bytes - leave has_temp 0. */
    if (n >= 11) {
        out->temp_c10 = (int)(short)(cogs_u16)(((cogs_u16)buf[10] << 8) | buf[9]);
        out->has_temp = 1;
    } else {
        out->temp_c10 = 0;
        out->has_temp = 0;
    }
    /* fw 0.8+ continues with VSYS mV (LE16) then uptime seconds (LE32). */
    if (n >= 17) {
        out->vsys_mv   = (cogs_u16)(((cogs_u16)buf[12] << 8) | buf[11]);
        out->uptime_s  = (cogs_u32)buf[13]
                       | ((cogs_u32)buf[14] << 8)
                       | ((cogs_u32)buf[15] << 16)
                       | ((cogs_u32)buf[16] << 24);
        out->has_vitals = 1;
    } else {
        out->vsys_mv    = 0;
        out->uptime_s   = 0;
        out->has_vitals = 0;
    }
    return COGS_OK;
}

int cogs_netinfo(cogs *c, cogs_netinfo_reply *out)
{
    cogs_u8  buf[72];      /* 29 + ssid (<=32) + ll 2 = 63 max; slack to 72 */
    cogs_u16 n;
    int      rc, i, sl;

    rc = cogs_send_frame(c, COGS_OP_NETINFO, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_NETINFO), buf, sizeof(buf), &n);
    if (rc != COGS_OK) {
        return rc;          /* COGS_ERR_PROTO if the card answered UNSUPPORTED */
    }
    if (n < 29) {
        return COGS_ERR_PROTO;
    }
    out->link_state = buf[0];
    for (i = 0; i < 4; i++) { out->ip[i]      = buf[1 + i];  }
    for (i = 0; i < 4; i++) { out->subnet[i]  = buf[5 + i];  }
    for (i = 0; i < 4; i++) { out->gateway[i] = buf[9 + i];  }
    for (i = 0; i < 4; i++) { out->dns1[i]    = buf[13 + i]; }
    for (i = 0; i < 4; i++) { out->dns2[i]    = buf[17 + i]; }
    for (i = 0; i < 6; i++) { out->mac[i]     = buf[21 + i]; }
    out->rssi = (signed char)buf[27];

    sl = buf[28];
    if (sl > 32) {
        sl = 32;
    }
    if ((cogs_u16)(29 + sl) > n) {     /* guard a short/garbled reply */
        sl = (int)n - 29;
    }
    if (sl < 0) {
        sl = 0;
    }
    for (i = 0; i < sl; i++) {
        out->ssid[i] = (char)buf[29 + i];
    }
    out->ssid[sl]  = '\0';
    out->ssid_len  = (cogs_u8)sl;
    /* Trailer after SSID: ll_active/ll_mode (0.60+), gs_ip[4] (0.61+). */
    out->ll_active = 0;
    out->ll_mode   = 0;
    for (i = 0; i < 4; i++) {
        out->gs_ip[i] = 0;
    }
    if ((cogs_u16)(29 + sl + 2) <= n) {
        out->ll_active = buf[29 + sl] ? 1u : 0u;
        out->ll_mode   = buf[29 + sl + 1];
    }
    if ((cogs_u16)(29 + sl + 6) <= n) {
        for (i = 0; i < 4; i++) {
            out->gs_ip[i] = buf[29 + sl + 2 + i];
        }
    }
    return COGS_OK;
}

/* Read a length-prefixed (u8) string at *off within buf[0..n) into out[cap],
 * NUL-terminating and advancing *off past it. Returns 0 on an overrun/short
 * frame (out is left as an empty string), 1 on success. */
static int read_lp_str(const cogs_u8 *buf, cogs_u16 n, cogs_u16 *off,
                       char *out, int cap)
{
    cogs_u8 m;
    int     i;

    out[0] = '\0';
    if (*off >= n) {
        return 0;
    }
    m = buf[(*off)++];
    if ((cogs_u16)(*off + m) > n) {
        return 0;
    }
    for (i = 0; i < (int)m && i < cap - 1; i++) {
        out[i] = (char)buf[*off + i];
    }
    out[i] = '\0';
    *off = (cogs_u16)(*off + m);
    return 1;
}

int cogs_tls_info(cogs *c, cogs_tls_reply *out)
{
    cogs_u8  buf[128];
    cogs_u16 n, off;
    int      rc;

    /* Clean defaults so a short/garbled/UNSUPPORTED answer reads as "no TLS". */
    out->valid = 0;
    out->verified = 0;
    out->version[0] = '\0';
    out->cipher[0]  = '\0';
    out->cn[0]      = '\0';

    rc = cogs_send_frame(c, COGS_OP_TLS_INFO, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_TLS_INFO), buf, sizeof(buf), &n);
    if (rc != COGS_OK) {
        return rc;          /* COGS_ERR_PROTO if the card answered UNSUPPORTED */
    }
    if (n < 2) {
        return COGS_ERR_PROTO;
    }
    out->valid    = buf[0];
    out->verified = buf[1];
    off = 2;
    read_lp_str(buf, n, &off, out->version, (int)sizeof out->version);
    read_lp_str(buf, n, &off, out->cipher,  (int)sizeof out->cipher);
    read_lp_str(buf, n, &off, out->cn,      (int)sizeof out->cn);
    return COGS_OK;
}

int cogs_card_info(cogs *c, cogs_cardinfo_reply *out)
{
    static const char HEX[] = "0123456789ABCDEF";
    cogs_u8  buf[24];
    cogs_u16 n;
    int      rc, i;

    /* Clean defaults so a short/UNSUPPORTED answer reads as "no identity". */
    for (i = 0; i < 8; i++) {
        out->serial[i] = 0;
    }
    out->serial_hex[0] = '\0';
    out->reset_reason  = 0;
    out->clock_hz      = 0;

    rc = cogs_send_frame(c, COGS_OP_CARD_INFO, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_CARD_INFO), buf, sizeof(buf), &n);
    if (rc != COGS_OK) {
        return rc;          /* COGS_ERR_PROTO if the card answered UNSUPPORTED */
    }
    if (n < 13) {
        return COGS_ERR_PROTO;
    }
    for (i = 0; i < 8; i++) {
        out->serial[i] = buf[i];
        out->serial_hex[i * 2]     = HEX[(buf[i] >> 4) & 0x0F];
        out->serial_hex[i * 2 + 1] = HEX[buf[i] & 0x0F];
    }
    out->serial_hex[16] = '\0';
    out->reset_reason   = buf[8];
    out->clock_hz       = (cogs_u32)buf[9]
                        | ((cogs_u32)buf[10] << 8)
                        | ((cogs_u32)buf[11] << 16)
                        | ((cogs_u32)buf[12] << 24);
    return COGS_OK;
}

int cogs_time(cogs *c, cogs_u32 *unix_out)
{
    cogs_u8  buf[8];
    cogs_u16 n;
    int      rc;

    rc = cogs_send_frame(c, COGS_OP_TIME, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_TIME), buf, sizeof(buf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 4) {
        return COGS_ERR_PROTO;
    }
    *unix_out = ((cogs_u32)buf[0]) |
                ((cogs_u32)buf[1] << 8) |
                ((cogs_u32)buf[2] << 16) |
                ((cogs_u32)buf[3] << 24);
    return COGS_OK;
}

/* Local strlen so the lib drops into a freestanding NDA without libc. */
static cogs_u16 z_len(const char *s)
{
    cogs_u16 n;

    n = 0;
    if (s != 0) {
        while (s[n] != '\0') {
            n++;
        }
    }
    return n;
}

/* ---- HTTP ----------------------------------------------------------------
 * Scratch buffers live at file scope, not on the stack: a CoGS client may run
 * as a freestanding NDA/CDA with a small stack, and a DATA event payload is up
 * to 1024 bytes. cogslib is single-threaded (one request at a time), so a
 * shared request-build buffer and a per-event receive buffer are safe. */
#define COGS_HTTP_REQ_MAX 2000   /* one inbound HTTP command frame (card cap <=2047) */
static cogs_u8 http_req[COGS_HTTP_REQ_MAX];   /* assembled HTTP command       */
static cogs_u8 http_fr[1024];                 /* one received event payload   */
static cogs_u8 json_fr[3 + 1024];             /* one $B2 frame: hdr + chunk    */

/* Cap on consecutive frames that make no progress on OUR request (DATA for a
 * different socket, or an unrelated event). A real response makes progress every
 * body frame, so this never trips a healthy fetch; it only bounds the case where
 * the card keeps emitting other traffic (e.g. a live volume's block frames) and
 * never completes our GET. Without it that loop can churn toward the 200000-frame
 * ceiling and read as a lockup. Far above any legitimate housekeeping burst. */
#define COGS_HTTP_NOPROGRESS_MAX 4096L

/* Run the response event loop: copy DATA body bytes into the caller buffer,
 * resolve on HTTP_DONE (success) or ERROR (card-level failure), skip anything
 * else. Bounded so a stream that never terminates still returns, and bailed out
 * early if nothing relevant to our socket arrives for a long stretch. */
static int http_collect(cogs *c, cogs_u8 sock,
                        cogs_u8 *body, cogs_u16 cap, cogs_http_reply *out)
{
    cogs_u32 frames;
    cogs_u32 noprog = 0;                      /* consecutive non-progress frames */
    cogs_u16 n, i;
    cogs_u8  op;
    int      rc;

    for (frames = 0; frames < 200000L; frames++) {
        rc = cogs_recv_frame(c, &op, http_fr, sizeof(http_fr), &n);
        if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
            return rc;                       /* timeout / transport failure   */
        }
        if (op == COGS_EV_DATA) {
            if (n >= 1 && http_fr[0] == sock) {
                noprog = 0;                  /* our body bytes: real progress  */
                for (i = 1; i < n; i++) {
                    if (out->body_len < cap) {
                        body[out->body_len] = http_fr[i];
                        out->body_len++;
                    } else {
                        out->truncated = 1;
                    }
                }
                continue;
            }
            if (++noprog > COGS_HTTP_NOPROGRESS_MAX) return COGS_ERR_TIMEOUT;
        } else if (op == COGS_EV_HTTP_DONE) {
            if (n >= 7 && http_fr[0] == sock) {
                out->status = (cogs_u16)(http_fr[1] | ((cogs_u16)http_fr[2] << 8));
                out->total  = (cogs_u32)http_fr[3] |
                              ((cogs_u32)http_fr[4] << 8) |
                              ((cogs_u32)http_fr[5] << 16) |
                              ((cogs_u32)http_fr[6] << 24);
                out->err = COGS_E_OK;
                return COGS_OK;
            }
            if (++noprog > COGS_HTTP_NOPROGRESS_MAX) return COGS_ERR_TIMEOUT;
        } else if (op == COGS_EV_ERROR) {
            out->err = (n >= 2) ? http_fr[1] : (cogs_u8)COGS_E_INTERNAL;
            return COGS_OK;                  /* completed, but the card failed */
        } else {
            /* an unrelated event (CONNECTED, WIFI_STATE, a live volume's frame,
             * ...) - keep reading, but do not churn here forever */
            if (++noprog > COGS_HTTP_NOPROGRESS_MAX) return COGS_ERR_TIMEOUT;
        }
    }
    return COGS_ERR_PROTO;
}

int cogs_http_begin(cogs *c, cogs_u8 sock, const char *method,
                    const char *url, const char *hdrs,
                    const cogs_u8 *body_in, cogs_u16 body_in_len,
                    cogs_u8 flags)
{
    cogs_u16 off, ml, ul, hl, i;

    ml = z_len(method);
    ul = z_len(url);
    hl = z_len(hdrs);
    if (ml == 0 || ml > 16 || ul == 0) {
        return COGS_ERR_OVERSIZE;
    }
    /* payload = sock,flags,method_len,method,url_len,url,
     *           hdrs_len(2),hdrs,body_len(2),body  ==  6 + ml + ul + hl + body */
    if ((cogs_u32)6 + ml + ul + hl + body_in_len > (cogs_u32)sizeof(http_req)) {
        return COGS_ERR_OVERSIZE;
    }

    off = 0;
    http_req[off++] = sock;
    http_req[off++] = flags;
    http_req[off++] = (cogs_u8)ml;
    for (i = 0; i < ml; i++) {
        http_req[off++] = (cogs_u8)method[i];
    }
    http_req[off++] = (cogs_u8)ul;
    for (i = 0; i < ul; i++) {
        http_req[off++] = (cogs_u8)url[i];
    }
    http_req[off++] = (cogs_u8)(hl & 0xFF);
    http_req[off++] = (cogs_u8)((hl >> 8) & 0xFF);
    for (i = 0; i < hl; i++) {
        http_req[off++] = (cogs_u8)hdrs[i];
    }
    http_req[off++] = (cogs_u8)(body_in_len & 0xFF);
    http_req[off++] = (cogs_u8)((body_in_len >> 8) & 0xFF);
    for (i = 0; i < body_in_len; i++) {
        http_req[off++] = body_in[i];
    }

    return cogs_send_frame(c, COGS_OP_HTTP, http_req, off);
}

int cogs_http_finish(cogs *c, cogs_u8 sock,
                     cogs_u8 *body_out, cogs_u16 cap, cogs_http_reply *out)
{
    out->status = 0;
    out->total = 0;
    out->body_len = 0;
    out->truncated = 0;
    out->err = COGS_E_OK;
    return http_collect(c, sock, body_out, cap, out);
}

int cogs_http_request(cogs *c, cogs_u8 sock, const char *method,
                      const char *url, const char *hdrs,
                      const cogs_u8 *body_in, cogs_u16 body_in_len,
                      cogs_u8 flags,
                      cogs_u8 *body_out, cogs_u16 cap, cogs_http_reply *out)
{
    int rc;

    rc = cogs_http_begin(c, sock, method, url, hdrs,
                         body_in, body_in_len, flags);
    if (rc != COGS_OK) {
        out->status = 0;
        out->total = 0;
        out->body_len = 0;
        out->truncated = 0;
        out->err = COGS_E_OK;
        return rc;
    }
    return cogs_http_finish(c, sock, body_out, cap, out);
}

int cogs_http_get(cogs *c, cogs_u8 sock, const char *url,
                  cogs_u8 *body_out, cogs_u16 cap, cogs_http_reply *out)
{
    return cogs_http_request(c, sock, "GET", url, 0, 0, 0, 0,
                             body_out, cap, out);
}

/* ---- JSON_GET ------------------------------------------------------------
 * Send sock,path_len,path and reassemble the $B2 reply. The card repeats
 * result + value_len(u16) in every chunk frame and concatenates the value
 * across frames; we copy up to 'cap' bytes and keep counting so truncation
 * and the full length are both reported. (Values are bounded by the spec's
 * u16 value_len, so the byte total matches value_len for everything we ship.)
 */
int cogs_json_get(cogs *c, cogs_u8 sock, const char *path,
                  cogs_u8 *out, cogs_u16 cap, cogs_json_reply *jr)
{
    cogs_u32 frames, seen;
    cogs_u16 pl, off, n, i;
    cogs_u8  op;
    int      rc;

    jr->value_len = 0;
    jr->copied = 0;
    jr->truncated = 0;
    jr->err = COGS_E_OK;

    pl = z_len(path);
    if (pl > 255) {
        return COGS_ERR_OVERSIZE;
    }
    off = 0;
    http_req[off++] = sock;
    http_req[off++] = (cogs_u8)pl;
    for (i = 0; i < pl; i++) {
        http_req[off++] = (cogs_u8)path[i];
    }

    rc = cogs_send_frame(c, COGS_OP_JSON_GET, http_req, off);
    if (rc != COGS_OK) {
        return rc;
    }

    seen = 0;
    for (frames = 0; frames < 200000L; frames++) {
        rc = cogs_recv_frame(c, &op, json_fr, sizeof(json_fr), &n);
        if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
            return rc;
        }
        if (op == COGS_RESP(COGS_OP_JSON_GET)) {
            if (n < 3) {
                return COGS_ERR_PROTO;
            }
            jr->err = json_fr[0];
            jr->value_len = (cogs_u16)(json_fr[1] | ((cogs_u16)json_fr[2] << 8));
            if (jr->err != COGS_E_OK) {
                return COGS_OK;              /* card-level failure, no value   */
            }
            for (i = 3; i < n; i++) {
                if (jr->copied < cap) {
                    out[jr->copied++] = json_fr[i];
                }
                seen++;
            }
            if (jr->value_len > cap) {
                jr->truncated = 1;
            }
            if (seen >= (cogs_u32)jr->value_len) {
                return COGS_OK;              /* all chunks reassembled         */
            }
            /* else more chunk frames coming; keep reading */
        } else if (op == COGS_EV_ERROR) {
            jr->err = (n >= 2) ? json_fr[1] : (cogs_u8)COGS_E_INTERNAL;
            return COGS_OK;
        }
        /* else an unrelated event; keep reading */
    }
    return COGS_ERR_PROTO;
}

int cogs_wifi_join(cogs *c, const char *ssid, const char *psk, cogs_u8 *result)
{
    /* Scratch lives in static storage, not on the stack: the same reason the
     * HTTP/JSON paths use static buffers. A Classic Desk Accessory (cogscda)
     * runs on a tiny stack, and a 160-byte frame buffer on top of the caller's
     * locals overran it and wedged the join. The lib is single-threaded /
     * non-reentrant, so a shared static buffer is safe here. */
    static cogs_u8 payload[160];
    cogs_u8  rbuf[4];
    cogs_u16 sl, pl, i, off, n;
    int      rc;

    sl = z_len(ssid);
    pl = z_len(psk);
    if (sl > 32 || pl > 63) {
        return COGS_ERR_OVERSIZE;
    }
    off = 0;
    payload[off++] = (cogs_u8)sl;
    for (i = 0; i < sl; i++) {
        payload[off++] = (cogs_u8)ssid[i];
    }
    payload[off++] = (cogs_u8)pl;
    for (i = 0; i < pl; i++) {
        payload[off++] = (cogs_u8)psk[i];
    }
    rc = cogs_send_frame(c, COGS_OP_WIFI_JOIN, payload, off);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_WIFI_JOIN), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* WIFI_SAVE ($04): persist the given credentials on the card (flash on real
 * firmware; RAM for the session on the emulator backend). Same payload as
 * join. *result is the card's E_* code ($84): E_OK saved, E_UNSUPPORTED if the
 * firmware cannot persist yet. */
int cogs_wifi_save(cogs *c, const char *ssid, const char *psk, cogs_u8 *result)
{
    static cogs_u8 payload[160];   /* off the (tiny CDA) stack; see wifi_join */
    cogs_u8  rbuf[4];
    cogs_u16 sl, pl, i, off, n;
    int      rc;

    sl = z_len(ssid);
    pl = z_len(psk);
    if (sl > 32 || pl > 63) {
        return COGS_ERR_OVERSIZE;
    }
    off = 0;
    payload[off++] = (cogs_u8)sl;
    for (i = 0; i < sl; i++) {
        payload[off++] = (cogs_u8)ssid[i];
    }
    payload[off++] = (cogs_u8)pl;
    for (i = 0; i < pl; i++) {
        payload[off++] = (cogs_u8)psk[i];
    }
    rc = cogs_send_frame(c, COGS_OP_WIFI_SAVE, payload, off);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_WIFI_SAVE), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* WIFI_SCAN ($02 -> $82): kick a scan and decode the network list. The radio
 * scan runs for a couple seconds on the card, so the reply lands far later than
 * a normal round trip - widen the spin cap for just this wait, then restore. */
int cogs_wifi_scan(cogs *c, cogs_scan_reply *out)
{
    static cogs_u8 buf[1 + COGS_SCAN_MAX * (3 + 32)];
    cogs_u32 saved_cap;
    cogs_u16 n, off;
    cogs_u8  count, k;
    int      rc, i, sl;

    out->count = 0;
    rc = cogs_send_frame(c, COGS_OP_WIFI_SCAN, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    saved_cap = c->spin_cap;
    c->spin_cap = saved_cap * 40;        /* ~ enough wall time for the scan */
    rc = recv_expected(c, COGS_RESP(COGS_OP_WIFI_SCAN), buf, sizeof(buf), &n);
    c->spin_cap = saved_cap;
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;                       /* PROTO if the card answered ERROR */
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    count = buf[0];
    off = 1;
    for (k = 0; k < count && out->count < COGS_SCAN_MAX; k++) {
        cogs_scan_net *e = &out->nets[out->count];
        if ((cogs_u16)(off + 3) > n) {
            break;                       /* truncated reply */
        }
        e->rssi = (signed char)buf[off++];
        e->auth = buf[off++];
        sl = buf[off++];
        if (sl > 32) {
            sl = 32;
        }
        if ((cogs_u16)(off + sl) > n) {
            break;
        }
        for (i = 0; i < sl; i++) {
            e->ssid[i] = (char)buf[off + i];
        }
        e->ssid[sl] = 0;
        e->ssid_len = (cogs_u8)sl;
        off += (cogs_u16)sl;
        out->count++;
    }
    return COGS_OK;
}

/* Async net-tool replies ($8B/$8C) can land seconds later and unrelated events
 * may arrive first, so mirror http_collect: spin for the expected opcode, skip
 * anything else, bound the loop so a silent card still returns. */
static cogs_u8 net_fr[2 + COGS_RESOLVE_MAX * 4 + 4];

static int net_reply_collect(cogs *c, cogs_u8 want, cogs_u16 *len)
{
    cogs_u32 frames;
    cogs_u32 saved_cap;
    cogs_u16 n;
    cogs_u8  op;
    int      rc;

    saved_cap   = c->spin_cap;
    c->spin_cap = COGS_ASYNC_SPIN;
    for (frames = 0; frames < 200000L; frames++) {
        rc = cogs_recv_frame(c, &op, net_fr, sizeof(net_fr), &n);
        if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
            c->spin_cap = saved_cap;
            return rc;
        }
        if (op == want) {
            *len = n;
            c->spin_cap = saved_cap;
            return rc;
        }
        if (op == COGS_EV_ERROR) {
            c->spin_cap = saved_cap;
            return COGS_ERR_PROTO;
        }
        /* else an unrelated event (WIFI_STATE, DATA, ...); keep reading */
    }
    c->spin_cap = saved_cap;
    return COGS_ERR_PROTO;
}

/* RESOLVE ($0B -> $8B): DNS A-lookup (nslookup). The card answers with result,
 * count, then count*addr[4]. Scratch is static: a CDA's stack is tiny and a
 * 255-byte payload on it wedged ping/nslookup the same way wifi_join did. */
int cogs_resolve(cogs *c, const char *host, cogs_resolve_reply *out)
{
    static cogs_u8 payload[255];
    cogs_u16 n, hl, i;
    int      rc;

    out->result = COGS_E_INTERNAL;
    out->count  = 0;

    hl = 0;
    while (host[hl] != '\0' && hl < 255) {
        payload[hl] = (cogs_u8)host[hl];
        hl++;
    }
    if (hl == 0) {
        return COGS_ERR_PROTO;
    }

    rc = cogs_send_frame(c, COGS_OP_RESOLVE, payload, hl);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = net_reply_collect(c, COGS_RESP(COGS_OP_RESOLVE), &n);
    if (rc != COGS_OK) {
        return rc;                       /* PROTO if the card answered ERROR */
    }
    if (n < 2) {
        return COGS_ERR_PROTO;
    }
    out->result = net_fr[0];
    out->count  = net_fr[1];
    if (out->count > COGS_RESOLVE_MAX) {
        out->count = COGS_RESOLVE_MAX;
    }
    for (i = 0; i < out->count; i++) {
        if ((cogs_u16)(2 + i * 4 + 4) > n) {
            out->count = (cogs_u8)i;     /* truncated reply */
            break;
        }
        out->addr[i][0] = net_fr[2 + i * 4 + 0];
        out->addr[i][1] = net_fr[2 + i * 4 + 1];
        out->addr[i][2] = net_fr[2 + i * 4 + 2];
        out->addr[i][3] = net_fr[2 + i * 4 + 3];
    }
    return COGS_OK;
}

/* TCP_PING ($0C -> $8C): time a TCP connect to host:port. The card resolves,
 * times the connect, drops the socket, and answers result, rtt_ms(u16), and the
 * resolved IP. Payload is static for the same CDA stack reason as RESOLVE. */
int cogs_probe(cogs *c, const char *host, cogs_u16 port, cogs_probe_reply *out)
{
    static cogs_u8 payload[2 + 255];
    cogs_u16 n, hl, i;
    int      rc;

    out->result = COGS_E_INTERNAL;
    out->rtt_ms = 0;
    out->addr[0] = out->addr[1] = out->addr[2] = out->addr[3] = 0;

    hl = 0;
    while (host[hl] != '\0' && hl < 255) {
        hl++;
    }
    if (hl == 0) {
        return COGS_ERR_PROTO;
    }
    payload[0] = (cogs_u8)(port & 0xFF);
    payload[1] = (cogs_u8)((port >> 8) & 0xFF);
    for (i = 0; i < hl; i++) {
        payload[2 + i] = (cogs_u8)host[i];
    }

    rc = cogs_send_frame(c, COGS_OP_TCP_PING, payload, (cogs_u16)(2 + hl));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = net_reply_collect(c, COGS_RESP(COGS_OP_TCP_PING), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 7) {
        return COGS_ERR_PROTO;
    }
    out->result  = net_fr[0];
    out->rtt_ms  = (cogs_u16)(net_fr[1] | ((cogs_u16)net_fr[2] << 8));
    out->addr[0] = net_fr[3];
    out->addr[1] = net_fr[4];
    out->addr[2] = net_fr[5];
    out->addr[3] = net_fr[6];
    return COGS_OK;
}

/* ICMP_PING ($0D -> $8D): a true ICMP echo ping (no port). Same $8C-shaped reply
 * as cogs_probe. A card without ICMP answers $93 UNSUPPORTED, which net_reply_-
 * collect surfaces as COGS_ERR_PROTO so the caller can fall back to cogs_probe.
 * Payload is static for the same CDA stack reason as the other net tools. */
int cogs_icmp_ping(cogs *c, const char *host, cogs_probe_reply *out)
{
    static cogs_u8 payload[255];
    cogs_u16 n, hl, i;
    int      rc;

    out->result = COGS_E_INTERNAL;
    out->rtt_ms = 0;
    out->addr[0] = out->addr[1] = out->addr[2] = out->addr[3] = 0;

    hl = 0;
    while (host[hl] != '\0' && hl < 255) {
        hl++;
    }
    if (hl == 0) {
        return COGS_ERR_PROTO;
    }
    for (i = 0; i < hl; i++) {
        payload[i] = (cogs_u8)host[i];
    }

    rc = cogs_send_frame(c, COGS_OP_ICMP_PING, payload, hl);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = net_reply_collect(c, COGS_RESP(COGS_OP_ICMP_PING), &n);
    if (rc != COGS_OK) {
        return rc;                       /* PROTO if the card has no ICMP */
    }
    if (n < 7) {
        return COGS_ERR_PROTO;
    }
    out->result  = net_fr[0];
    out->rtt_ms  = (cogs_u16)(net_fr[1] | ((cogs_u16)net_fr[2] << 8));
    out->addr[0] = net_fr[3];
    out->addr[1] = net_fr[4];
    out->addr[2] = net_fr[5];
    out->addr[3] = net_fr[6];
    return COGS_OK;
}

/* TRACEROUTE ($0E -> $8E): probe one hop (ttl) toward host. Payload is ttl then
 * the host bytes; the $8E reply is result, ttl, addr[4], rtt_ms(u16), flags. The
 * caller loops ttl until COGS_TR_DONE or a hop cap. Static payload for the same
 * CDA stack reason as the other net tools; COGS_ERR_PROTO if the card lacks it. */
int cogs_traceroute(cogs *c, const char *host, cogs_u8 ttl, cogs_trace_reply *out)
{
    static cogs_u8 payload[1 + 255];
    cogs_u16 n, hl, i;
    int      rc;

    out->result = COGS_E_INTERNAL;
    out->ttl    = ttl;
    out->rtt_ms = 0;
    out->flags  = 0;
    out->addr[0] = out->addr[1] = out->addr[2] = out->addr[3] = 0;

    hl = 0;
    while (host[hl] != '\0' && hl < 254) {
        hl++;
    }
    if (hl == 0) {
        return COGS_ERR_PROTO;
    }
    payload[0] = ttl;
    for (i = 0; i < hl; i++) {
        payload[1 + i] = (cogs_u8)host[i];
    }

    rc = cogs_send_frame(c, COGS_OP_TRACEROUTE, payload, (cogs_u16)(1 + hl));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = net_reply_collect(c, COGS_RESP(COGS_OP_TRACEROUTE), &n);
    if (rc != COGS_OK) {
        return rc;                       /* PROTO if the card has no traceroute */
    }
    if (n < 9) {
        return COGS_ERR_PROTO;
    }
    out->result  = net_fr[0];
    out->ttl     = net_fr[1];
    out->addr[0] = net_fr[2];
    out->addr[1] = net_fr[3];
    out->addr[2] = net_fr[4];
    out->addr[3] = net_fr[5];
    out->rtt_ms  = (cogs_u16)(net_fr[6] | ((cogs_u16)net_fr[7] << 8));
    out->flags   = net_fr[8];
    return COGS_OK;
}

/* PREFS_GET ($06): read the GS-owned settings blob the card holds in NV. The
 * response is [result][blob...]; the blob length is the frame length minus the
 * result byte. *result is E_UNSUPPORTED on firmware that cannot persist yet -
 * callers treat that (and an empty blob) as "no saved prefs". */
int cogs_prefs_get(cogs *c, cogs_u8 *out, cogs_u16 cap, cogs_u16 *len,
                   cogs_u8 *result)
{
    static cogs_u8 rbuf[1 + COGS_PREFS_MAX];
    cogs_u16 n, i, vlen;
    int      rc;

    *len = 0;
    rc = cogs_send_frame(c, COGS_OP_PREFS_GET, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_PREFS_GET), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    vlen = (cogs_u16)(n - 1);
    if (vlen > cap) {
        vlen = cap;
    }
    for (i = 0; i < vlen; i++) {
        out[i] = rbuf[1 + i];
    }
    *len = vlen;
    return COGS_OK;
}

/* PREFS_SET ($07): store the GS-owned settings blob (<= COGS_PREFS_MAX bytes)
 * on the card. *result is the card's E_* ($87): E_OK stored, E_UNSUPPORTED if
 * the firmware cannot persist yet. */
int cogs_prefs_set(cogs *c, const cogs_u8 *data, cogs_u8 len, cogs_u8 *result)
{
    cogs_u8  rbuf[4];
    cogs_u16 n;
    int      rc;

    if (len > COGS_PREFS_MAX) {
        return COGS_ERR_OVERSIZE;
    }
    rc = cogs_send_frame(c, COGS_OP_PREFS_SET, data, len);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_PREFS_SET), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* ---- link layer ----------------------------------------------------------
 * LINK_OPEN ($40 -> $C0) puts the card in NIC mode; LINK_FRAME ($41) is a
 * fire-and-forget outbound frame; LINK_CLOSE ($42 -> $C2) leaves NIC mode.
 * Replies follow the universal 0x80|opcode rule (the spec's draft $C4/$C5 were
 * a numbering slip - see spec/cogs-spec-v0.6-notes.md). Inbound frames are not
 * polled here; they surface as $96 events via cogs_poll_event. */
int cogs_link_open(cogs *c, cogs_u8 mode, cogs_u8 *result)
{
    cogs_u8  payload[1];
    cogs_u8  rbuf[4];
    cogs_u16 n;
    int      rc;

    payload[0] = mode;
    rc = cogs_send_frame(c, COGS_OP_LINK_OPEN, payload, 1);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_LINK_OPEN), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;          /* COGS_ERR_PROTO if the card answered UNSUPPORTED */
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

int cogs_link_frame(cogs *c, const cogs_u8 *frame, cogs_u16 len)
{
    /* Fire-and-forget: push the frame and return. The card emits no per-frame
     * ack (spec section 12), so there is nothing to receive here - draining a
     * reply would serialize the link to one packet per round trip. */
    return cogs_send_frame(c, COGS_OP_LINK_FRAME, frame, len);
}

int cogs_link_close(cogs *c, cogs_u8 *result)
{
    cogs_u8  rbuf[4];
    cogs_u16 n;
    int      rc;

    rc = cogs_send_frame(c, COGS_OP_LINK_CLOSE, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_LINK_CLOSE), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

int cogs_link_ip(cogs *c, const cogs_u8 ip[4], cogs_u8 *result)
{
    cogs_u8  rbuf[4];
    cogs_u16 n;
    int      rc;

    rc = cogs_send_frame(c, COGS_OP_LINK_IP, ip, 4);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_LINK_IP), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* ---- network storage (spec section 13) ----------------------------------
 * VOL_OPEN/READ/CLOSE. Like the net tools, the card resolves + connects + runs
 * the HTTP range fetch before it answers, so the open/read waits widen the spin
 * cap. The read reply carries a whole 512-byte block after a 6-byte head, so it
 * gets its own static receive buffer (off the tiny CDA/NDA stack, same reason
 * as the HTTP/net-tool scratch). */
static cogs_u8 vol_fr[6 + COGS_VOL_BLOCK];

static int vol_collect(cogs *c, cogs_u8 want, cogs_u16 *len)
{
    cogs_u32 frames;
    cogs_u32 saved_cap;
    cogs_u16 n;
    cogs_u8  op;
    int      rc;

    saved_cap   = c->spin_cap;
    c->spin_cap = COGS_ASYNC_SPIN;
    for (frames = 0; frames < 200000L; frames++) {
        rc = cogs_recv_frame(c, &op, vol_fr, sizeof(vol_fr), &n);
        if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
            c->spin_cap = saved_cap;
            return rc;
        }
        if (op == want) {
            *len = n;
            c->spin_cap = saved_cap;
            return rc;
        }
        if (op == COGS_EV_ERROR) {
            c->spin_cap = saved_cap;
            return COGS_ERR_PROTO;        /* card answered UNSUPPORTED/etc.  */
        }
        /* else an unrelated event (WIFI_STATE, ...); keep reading */
    }
    c->spin_cap = saved_cap;
    return COGS_ERR_PROTO;
}

int cogs_vol_open(cogs *c, cogs_u8 handle, cogs_u8 flags, const char *url,
                  cogs_vol_reply *out)
{
    static cogs_u8 payload[3 + 255];   /* handle, flags, url_len, url (<=255) */
    cogs_u16 n, ul, i;
    int      rc;

    out->handle       = handle;
    out->result       = COGS_E_INTERNAL;
    out->total_blocks = 0;

    ul = z_len(url);
    if (ul == 0 || ul > 255) {
        return COGS_ERR_OVERSIZE;
    }
    payload[0] = handle;
    payload[1] = flags;
    payload[2] = (cogs_u8)ul;
    for (i = 0; i < ul; i++) {
        payload[3 + i] = (cogs_u8)url[i];
    }

    rc = cogs_send_frame(c, COGS_OP_VOL_OPEN, payload, (cogs_u16)(3 + ul));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = vol_collect(c, COGS_RESP(COGS_OP_VOL_OPEN), &n);
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;                  /* PROTO if the card answered UNSUPPORTED */
    }
    if (n < 6) {
        return COGS_ERR_PROTO;
    }
    out->handle       = vol_fr[0];
    out->result       = vol_fr[1];
    out->total_blocks = (cogs_u32)vol_fr[2]
                      | ((cogs_u32)vol_fr[3] << 8)
                      | ((cogs_u32)vol_fr[4] << 16)
                      | ((cogs_u32)vol_fr[5] << 24);
    return COGS_OK;
}

int cogs_vol_read(cogs *c, cogs_u8 handle, cogs_u32 block,
                  cogs_u8 *out, cogs_u8 *result)
{
    cogs_u8  payload[5];
    cogs_u16 n, i;
    int      rc;

    *result = COGS_E_INTERNAL;
    payload[0] = handle;
    payload[1] = (cogs_u8)(block & 0xFF);
    payload[2] = (cogs_u8)((block >> 8) & 0xFF);
    payload[3] = (cogs_u8)((block >> 16) & 0xFF);
    payload[4] = (cogs_u8)((block >> 24) & 0xFF);

    rc = cogs_send_frame(c, COGS_OP_VOL_READ, payload, 5);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = vol_collect(c, COGS_RESP(COGS_OP_VOL_READ), &n);
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;
    }
    if (n < 6) {
        return COGS_ERR_PROTO;
    }
    *result = vol_fr[1];
    if (vol_fr[1] == COGS_E_OK) {
        if (n < (cogs_u16)(6 + COGS_VOL_BLOCK)) {
            return COGS_ERR_PROTO;       /* OK result must carry the block   */
        }
        for (i = 0; i < COGS_VOL_BLOCK; i++) {
            out[i] = vol_fr[6 + i];
        }
    }
    return COGS_OK;
}

int cogs_vol_close(cogs *c, cogs_u8 handle, cogs_u8 *result)
{
    cogs_u8  payload[1];
    cogs_u8  rbuf[4];
    cogs_u16 n;
    int      rc;

    *result = COGS_E_INTERNAL;
    payload[0] = handle;
    rc = cogs_send_frame(c, COGS_OP_VOL_CLOSE, payload, 1);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_VOL_CLOSE), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 2) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[1];               /* $D2 payload: handle(1), result(1)    */
    return COGS_OK;
}

/* VOL_URL_SET ($53 -> $D3): persist the boot/default volume URL in card NV.
 * Wire payload is url_len(1) + url bytes (url_len 0 clears it). Reply is the
 * card's E_* in one byte. */
int cogs_vol_url_set(cogs *c, const char *url, cogs_u8 *result)
{
    static cogs_u8 payload[1 + COGS_BOOT_URL_MAX];
    cogs_u8  rbuf[4];
    cogs_u16 n, ulen, i;
    int      rc;

    *result = COGS_E_INTERNAL;
    ulen = 0;
    if (url) {
        while (url[ulen] && ulen <= COGS_BOOT_URL_MAX) {
            ulen++;
        }
    }
    if (ulen > COGS_BOOT_URL_MAX) {
        return COGS_ERR_OVERSIZE;
    }
    payload[0] = (cogs_u8)ulen;
    for (i = 0; i < ulen; i++) {
        payload[1 + i] = (cogs_u8)url[i];
    }
    rc = cogs_send_frame(c, COGS_OP_VOL_URL_SET, payload, (cogs_u16)(1 + ulen));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_VOL_URL_SET), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* VOL_URL_GET ($54 -> $D4): read the stored boot/default URL. Reply payload is
 * url_len(1) + url bytes. A card too old to store it answers an error frame
 * (recv_expected -> COGS_ERR_PROTO); report that as UNSUPPORTED + empty rather
 * than a transport failure, so a config screen can just show "(none)". */
int cogs_vol_url_get(cogs *c, char *out, cogs_u16 cap, cogs_u16 *len,
                     cogs_u8 *result)
{
    static cogs_u8 rbuf[1 + COGS_BOOT_URL_MAX];
    cogs_u16 n, ulen, i;
    int      rc;

    *len = 0;
    *result = COGS_E_OK;
    if (cap > 0) {
        out[0] = 0;
    }
    rc = cogs_send_frame(c, COGS_OP_VOL_URL_GET, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_VOL_URL_GET), rbuf, sizeof(rbuf), &n);
    if (rc == COGS_ERR_PROTO) {
        *result = COGS_E_UNSUPPORTED;     /* old card: no boot-URL store */
        return COGS_OK;
    }
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    ulen = rbuf[0];
    if (ulen > (cogs_u16)(n - 1)) {
        ulen = (cogs_u16)(n - 1);         /* trust the frame over the count   */
    }
    if (cap == 0) {
        *len = 0;
        return COGS_OK;
    }
    if (ulen > (cogs_u16)(cap - 1)) {
        ulen = (cogs_u16)(cap - 1);       /* leave room for the NUL           */
    }
    for (i = 0; i < ulen; i++) {
        out[i] = (char)rbuf[1 + i];
    }
    out[ulen] = 0;
    *len = ulen;
    return COGS_OK;
}

/* CAT_URL_SET ($59 -> $D9): persist the shared catalog/browse base URL (the same
 * NV slot the ROM config Image Source page uses). Wire payload is url_len(1) +
 * url bytes (url_len 0 reverts to the default). Reply is the card's E_*. */
int cogs_cat_url_set(cogs *c, const char *url, cogs_u8 *result)
{
    static cogs_u8 payload[1 + COGS_BOOT_URL_MAX];
    cogs_u8  rbuf[4];
    cogs_u16 n, ulen, i;
    int      rc;

    *result = COGS_E_INTERNAL;
    ulen = 0;
    if (url) {
        while (url[ulen] && ulen <= COGS_BOOT_URL_MAX) {
            ulen++;
        }
    }
    if (ulen > COGS_BOOT_URL_MAX) {
        return COGS_ERR_OVERSIZE;
    }
    payload[0] = (cogs_u8)ulen;
    for (i = 0; i < ulen; i++) {
        payload[1 + i] = (cogs_u8)url[i];
    }
    rc = cogs_send_frame(c, COGS_OP_CAT_URL_SET, payload, (cogs_u16)(1 + ulen));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_CAT_URL_SET), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* CAT_URL_GET ($5A -> $DA): read the shared catalog base URL. Reply payload is
 * url_len(1) + url bytes. An old card answers an error frame; report it as
 * UNSUPPORTED + empty (not a transport failure) so the caller falls back to the
 * compiled default. */
int cogs_cat_url_get(cogs *c, char *out, cogs_u16 cap, cogs_u16 *len,
                     cogs_u8 *result)
{
    static cogs_u8 rbuf[1 + COGS_BOOT_URL_MAX];
    cogs_u16 n, ulen, i;
    int      rc;

    *len = 0;
    *result = COGS_E_OK;
    if (cap > 0) {
        out[0] = 0;
    }
    rc = cogs_send_frame(c, COGS_OP_CAT_URL_GET, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_CAT_URL_GET), rbuf, sizeof(rbuf), &n);
    if (rc == COGS_ERR_PROTO) {
        *result = COGS_E_UNSUPPORTED;     /* old card: no catalog-URL store */
        return COGS_OK;
    }
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    ulen = rbuf[0];
    if (ulen > (cogs_u16)(n - 1)) {
        ulen = (cogs_u16)(n - 1);
    }
    if (cap == 0) {
        *len = 0;
        return COGS_OK;
    }
    if (ulen > (cogs_u16)(cap - 1)) {
        ulen = (cogs_u16)(cap - 1);
    }
    for (i = 0; i < ulen; i++) {
        out[i] = (char)rbuf[1 + i];
    }
    out[ulen] = 0;
    *len = ulen;
    return COGS_OK;
}

/* VOL_MOUNT_SET ($55 -> $D5): persist unit's mount URL in card NV. Wire payload
 * is unit(1) + url_len(1) + url bytes (url_len 0 clears the slot). Reply is the
 * card's E_* in one byte. */
int cogs_vol_mount_set(cogs *c, cogs_u8 unit, const char *url, cogs_u8 *result)
{
    static cogs_u8 payload[2 + COGS_BOOT_URL_MAX];
    cogs_u8  rbuf[4];
    cogs_u16 n, ulen, i;
    int      rc;

    *result = COGS_E_INTERNAL;
    ulen = 0;
    if (url) {
        while (url[ulen] && ulen <= COGS_BOOT_URL_MAX) {
            ulen++;
        }
    }
    if (ulen > COGS_BOOT_URL_MAX) {
        return COGS_ERR_OVERSIZE;
    }
    payload[0] = unit;
    payload[1] = (cogs_u8)ulen;
    for (i = 0; i < ulen; i++) {
        payload[2 + i] = (cogs_u8)url[i];
    }
    rc = cogs_send_frame(c, COGS_OP_VOL_MOUNT_SET, payload, (cogs_u16)(2 + ulen));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_VOL_MOUNT_SET), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

/* VOL_MOUNT_GET ($56 -> $D6): read unit's stored mount URL. Reply payload is
 * result(1) + unit(1) + url_len(1) + url bytes. An old card answers an error
 * frame; report that as UNSUPPORTED + empty so a config screen shows "(none)". */
int cogs_vol_mount_get(cogs *c, cogs_u8 unit, char *out, cogs_u16 cap,
                       cogs_u16 *len, cogs_u8 *result)
{
    static cogs_u8 rbuf[3 + COGS_BOOT_URL_MAX];
    cogs_u8  req[1];
    cogs_u16 n, ulen, i;
    int      rc;

    *len = 0;
    *result = COGS_E_OK;
    if (cap > 0) {
        out[0] = 0;
    }
    req[0] = unit;
    rc = cogs_send_frame(c, COGS_OP_VOL_MOUNT_GET, req, 1);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_VOL_MOUNT_GET), rbuf, sizeof(rbuf), &n);
    if (rc == COGS_ERR_PROTO) {
        *result = COGS_E_UNSUPPORTED;     /* old card: no mount table */
        return COGS_OK;
    }
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;
    }
    if (n < 3) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];                    /* card's E_* for the unit          */
    ulen = rbuf[2];                       /* rbuf[1] echoes the unit          */
    if (ulen > (cogs_u16)(n - 3)) {
        ulen = (cogs_u16)(n - 3);         /* trust the frame over the count   */
    }
    if (cap == 0) {
        *len = 0;
        return COGS_OK;
    }
    if (ulen > (cogs_u16)(cap - 1)) {
        ulen = (cogs_u16)(cap - 1);       /* leave room for the NUL           */
    }
    for (i = 0; i < ulen; i++) {
        out[i] = (char)rbuf[3 + i];
    }
    out[ulen] = 0;
    *len = ulen;
    return COGS_OK;
}

/* ---- non-blocking event pump --------------------------------------------
 * Peek STATUS once; only read a frame if one is actually waiting. This is the
 * one place cogslib deliberately does NOT spin: the link layer calls it on
 * every idle tick and must return immediately when the radio is quiet. When a
 * frame is present we hand off to cogs_recv_frame, whose per-byte waits now
 * resolve instantly because RX_AVAIL is already set. */
int cogs_poll_event(cogs *c, cogs_u8 *opcode, cogs_u8 *payload,
                    cogs_u16 cap, cogs_u16 *len)
{
    cogs_io *io = c->io;
    int      s;

    *len = 0;
    s = io->rd(io, COGS_REG_STATUS);
    if (!(s & COGS_ST_RX_AVAIL)) {
        return COGS_NO_EVENT;
    }
    return cogs_recv_frame(c, opcode, payload, cap, len);
}

/* ---- audio catalog base URL (fw 0.93) ------------------------------------
 * Same wire shape and old-card degradation as the cat_url pair above. */

int cogs_aud_url_set(cogs *c, const char *url, cogs_u8 *result)
{
    static cogs_u8 payload[1 + COGS_BOOT_URL_MAX];
    cogs_u8  rbuf[4];
    cogs_u16 n, ulen, i;
    int      rc;

    *result = COGS_E_INTERNAL;
    ulen = 0;
    if (url) {
        while (url[ulen] && ulen <= COGS_BOOT_URL_MAX) {
            ulen++;
        }
    }
    if (ulen > COGS_BOOT_URL_MAX) {
        return COGS_ERR_OVERSIZE;
    }
    payload[0] = (cogs_u8)ulen;
    for (i = 0; i < ulen; i++) {
        payload[1 + i] = (cogs_u8)url[i];
    }
    rc = cogs_send_frame(c, COGS_OP_AUD_URL_SET, payload, (cogs_u16)(1 + ulen));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_AUD_URL_SET), rbuf, sizeof(rbuf), &n);
    if (rc == COGS_ERR_PROTO) {
        *result = COGS_E_UNSUPPORTED;    /* old card: no audio-URL store */
        return COGS_OK;
    }
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[0];
    return COGS_OK;
}

int cogs_aud_url_get(cogs *c, char *out, cogs_u16 cap, cogs_u16 *len,
                     cogs_u8 *result)
{
    static cogs_u8 rbuf[1 + COGS_BOOT_URL_MAX];
    cogs_u16 n, ulen, i;
    int      rc;

    *len = 0;
    *result = COGS_E_OK;
    if (cap > 0) {
        out[0] = 0;
    }
    rc = cogs_send_frame(c, COGS_OP_AUD_URL_GET, 0, 0);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_AUD_URL_GET), rbuf, sizeof(rbuf), &n);
    if (rc == COGS_ERR_PROTO) {
        *result = COGS_E_UNSUPPORTED;    /* old card: no audio-URL store */
        return COGS_OK;
    }
    if (rc != COGS_OK && rc != COGS_ERR_OVERSIZE) {
        return rc;
    }
    if (n < 1) {
        return COGS_ERR_PROTO;
    }
    ulen = rbuf[0];
    if (ulen > (cogs_u16)(n - 1)) {
        ulen = (cogs_u16)(n - 1);
    }
    if (cap == 0) {
        *len = 0;
        return COGS_OK;
    }
    if (ulen > (cogs_u16)(cap - 1)) {
        ulen = (cogs_u16)(cap - 1);
    }
    for (i = 0; i < ulen; i++) {
        out[i] = (char)rbuf[1 + i];
    }
    out[ulen] = 0;
    *len = ulen;
    return COGS_OK;
}

/* ---- byte streaming (fw 0.93) ---------------------------------------------
 * STREAM_READ is the hot path of audio playback and gets its own receive
 * plumbing: the generic recv_expected drains the frame into a bounce buffer
 * and the caller copies the payload out again, and at ORCA/C -O0 that extra
 * 1.5 KB indexed copy (plus everything else per pass) held the whole produce
 * loop to ~8 KB/s - BELOW the 21.9 KB/s the DOC drinks, so the ring drained
 * on air (2026-08-21 headless runs: Lead 0K, stuttering). Here the 6-byte
 * status header lands in a scratch buffer and the audio bytes burst straight
 * into the caller's ring via io->rdn. */

/* Count-gated payload drain into dst (cap bytes; excess is popped and
 * dropped to stay frame-aligned). Same logic as the cogs_recv_frame body. */
static int drain_bytes(cogs *c, cogs_u8 *dst, cogs_u16 cap, cogs_u16 n)
{
    cogs_io *io = c->io;
    cogs_u8  lo, hi, b;
    cogs_u16 i, avail, take;
    int      over;

    over = 0;
    i = 0;
    while (i < n) {
        lo = (cogs_u8)io->rd(io, COGS_REG_RXLO);   /* latches RXHI */
        hi = (cogs_u8)io->rd(io, COGS_REG_RXHI);
        avail = (cogs_u16)(((cogs_u16)hi << 8) | lo);
        if (avail == 0) {
            if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
                return COGS_ERR_TIMEOUT;
            }
            continue;
        }
        take = (cogs_u16)(n - i);
        if (avail < take) {
            take = avail;
        }
        if (io->rdn != 0 && (cogs_u32)i + take <= (cogs_u32)cap) {
            io->rdn(io, dst + i, take);
            i = (cogs_u16)(i + take);
        } else {
            while (take != 0) {
                b = (cogs_u8)io->rd(io, COGS_REG_DATA);
                if (i < cap) {
                    dst[i] = b;
                } else {
                    over = 1;
                }
                i++;
                take--;
            }
        }
    }
    return over ? COGS_ERR_OVERSIZE : COGS_OK;
}

int cogs_stream_open(cogs *c, const char *url, cogs_u8 *result)
{
    static cogs_u8 payload[1 + COGS_BOOT_URL_MAX];
    cogs_u8  rbuf[4];
    cogs_u16 n, ulen, i;
    int      rc;

    *result = COGS_E_INTERNAL;
    ulen = 0;
    if (url) {
        while (url[ulen] && ulen <= COGS_BOOT_URL_MAX) {
            ulen++;
        }
    }
    if (ulen == 0) {
        return COGS_ERR_PROTO;
    }
    if (ulen > COGS_BOOT_URL_MAX) {
        return COGS_ERR_OVERSIZE;
    }
    payload[0] = (cogs_u8)ulen;
    for (i = 0; i < ulen; i++) {
        payload[1 + i] = (cogs_u8)url[i];
    }
    rc = cogs_send_frame(c, COGS_OP_STREAM_OPEN, payload, (cogs_u16)(1 + ulen));
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_STREAM_OPEN), rbuf, sizeof(rbuf), &n);
    if (rc == COGS_ERR_PROTO) {
        *result = COGS_E_UNSUPPORTED;    /* old card: no streaming (probe) */
        return COGS_OK;
    }
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 2) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[1];                   /* $F0 payload: handle(1), result(1) */
    return COGS_OK;
}

int cogs_stream_read(cogs *c, cogs_u8 *out, cogs_u16 maxlen, cogs_u16 *len,
                     cogs_u8 *flags, cogs_u8 *level, cogs_u8 *result)
{
    static cogs_u8 skipbuf[64];          /* events that jump the queue */
    cogs_io *io = c->io;
    cogs_u8  payload[3];
    cogs_u8  hdr[6];
    cogs_u8  op;
    cogs_u16 n, dlen;
    int      rc, skips;

    *len = 0;
    *flags = 0;
    *level = 0;
    *result = COGS_E_INTERNAL;
    if (maxlen > COGS_STREAM_READ_MAX) {
        maxlen = COGS_STREAM_READ_MAX;
    }
    payload[0] = 0;                      /* handle */
    payload[1] = (cogs_u8)(maxlen & 0xFF);
    payload[2] = (cogs_u8)((maxlen >> 8) & 0xFF);
    rc = cogs_send_frame(c, COGS_OP_STREAM_READ, payload, 3);
    if (rc != COGS_OK) {
        return rc;
    }

    for (skips = 0; skips < 16; skips++) {
        if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
            return COGS_ERR_TIMEOUT;
        }
        op = (cogs_u8)io->rd(io, COGS_REG_DATA);
        if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
            return COGS_ERR_TIMEOUT;
        }
        n = (cogs_u16)io->rd(io, COGS_REG_DATA);
        if (wait_status(c, COGS_ST_RX_AVAIL) != COGS_OK) {
            return COGS_ERR_TIMEOUT;
        }
        n |= (cogs_u16)io->rd(io, COGS_REG_DATA) << 8;

        if (op != COGS_RESP(COGS_OP_STREAM_READ)) {
            rc = drain_bytes(c, skipbuf, sizeof(skipbuf), n);
            if (rc == COGS_ERR_TIMEOUT) {
                return rc;
            }
            if (op == COGS_EV_ERROR) {
                return COGS_ERR_PROTO;
            }
            continue;                    /* unrelated event; read the next */
        }

        if (n < 6) {
            drain_bytes(c, skipbuf, sizeof(skipbuf), n);
            return COGS_ERR_PROTO;
        }
        rc = drain_bytes(c, hdr, 6, 6);
        if (rc != COGS_OK) {
            return rc;
        }
        dlen = (cogs_u16)hdr[4] | ((cogs_u16)hdr[5] << 8);
        if (dlen > (cogs_u16)(n - 6) || dlen > maxlen) {
            drain_bytes(c, skipbuf, sizeof(skipbuf), (cogs_u16)(n - 6));
            return COGS_ERR_PROTO;
        }
        rc = drain_bytes(c, out, dlen, dlen);   /* payload straight to caller */
        if (rc != COGS_OK) {
            return rc;
        }
        if ((cogs_u16)(n - 6) > dlen) {         /* trailing slop, drop it */
            drain_bytes(c, skipbuf, sizeof(skipbuf),
                        (cogs_u16)(n - 6 - dlen));
        }
        *result = hdr[1];
        *flags  = hdr[2];
        *level  = hdr[3];
        *len = dlen;
        return COGS_OK;
    }
    return COGS_ERR_PROTO;
}

int cogs_stream_close(cogs *c, cogs_u8 *result)
{
    cogs_u8  payload[1];
    cogs_u8  rbuf[4];
    cogs_u16 n;
    int      rc;

    *result = COGS_E_INTERNAL;
    payload[0] = 0;                      /* handle */
    rc = cogs_send_frame(c, COGS_OP_STREAM_CLOSE, payload, 1);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_STREAM_CLOSE), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 2) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[1];
    return COGS_OK;
}

int cogs_stream_stat(cogs *c, cogs_u8 *state, cogs_u8 *level,
                     cogs_u32 *buffered, cogs_u32 *total, cogs_u8 *result)
{
    cogs_u8  payload[1];
    cogs_u8  rbuf[14];
    cogs_u16 n;
    int      rc;

    *state = 0;
    *level = 0;
    *buffered = 0;
    *total = 0;
    *result = COGS_E_INTERNAL;
    payload[0] = 0;                      /* handle */
    rc = cogs_send_frame(c, COGS_OP_STREAM_STAT, payload, 1);
    if (rc != COGS_OK) {
        return rc;
    }
    rc = recv_expected(c, COGS_RESP(COGS_OP_STREAM_STAT), rbuf, sizeof(rbuf), &n);
    if (rc != COGS_OK) {
        return rc;
    }
    if (n < 12) {
        return COGS_ERR_PROTO;
    }
    *result = rbuf[1];
    *state  = rbuf[2];
    *level  = rbuf[3];
    *buffered = (cogs_u32)rbuf[4] | ((cogs_u32)rbuf[5] << 8) |
                ((cogs_u32)rbuf[6] << 16) | ((cogs_u32)rbuf[7] << 24);
    *total    = (cogs_u32)rbuf[8] | ((cogs_u32)rbuf[9] << 8) |
                ((cogs_u32)rbuf[10] << 16) | ((cogs_u32)rbuf[11] << 24);
    return COGS_OK;
}
