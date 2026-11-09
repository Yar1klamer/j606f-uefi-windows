// J606F Wi-Fi step G: association, WPA2-PSK and the HTT data path (ath10k mac.c, wmi-tlv.c, htt_tx.c, htt_rx.c), in Ethernet frame mode.
//  802.11a/b/g + HT and VHT (1 stream, up to 80 MHz; C:\j606f\drv\wifi.legacy forces legacy rates). The network comes from the owner's C:\j606f\drv\wifi.conf (ssid=, psk=).
//  SSID, BSSID, MAC addresses, the passphrase and all keys are never logged: only states, status codes, counts, lengths and ethertypes.
typedef struct {
    UCHAR mac[6];
    void (*rx)(void *ctx, const UCHAR *f, ULONG n);
    void (*link)(void *ctx, int up);
    void *ctx;
    int (*tx)(const UCHAR *f, ULONG n);
    // the fields below exist only for the Native Wi-Fi miniport (bound with op 2, g_nv): Windows drives scans, connections and the WPA2 handshake
    void (*bss)(void *ctx, const UCHAR *f, ULONG n, ULONG chan, LONG rssi);
    void (*scan_done)(void *ctx);
    void (*assoc)(void *ctx, ULONG status, const UCHAR *req, ULONG reqn, const UCHAR *resp, ULONG respn);
    void (*lost)(void *ctx, ULONG reason);
    int (*scan)(void);
    int (*connect)(const UCHAR *f, ULONG n, ULONG chan, ULONG rsn);
    void (*disconnect)(void);
    void (*key)(ULONG idx, ULONG group, const UCHAR *k, ULONG n);
    ULONG mbps;                                         // link rate of the current connection, written before assoc()
    LONG rssi;                                          // dBm of the last received PPDU, 0 = unknown
} J606F_WIFI_BIND;

NTSTATUS ExCreateCallback(PVOID *CallbackObject, POBJECT_ATTRIBUTES Attr, BOOLEAN Create, BOOLEAN AllowMultiple);
PVOID ExRegisterCallback(PVOID CallbackObject, void (*Fn)(PVOID, PVOID, PVOID), PVOID Context);

enum { NS_OFF, NS_VSTART, NS_PEER, NS_AUTH, NS_ASSOC, NS_4WAY, NS_WAITM4, NS_UP, NS_FAIL };
#define NET_TXN   128u
#define NET_QN    256u

static J606F_WIFI_BIND * volatile g_nb;
static int g_nv_wanted;                  // C:\\j606f\\drv\\wifi.on: the Native Wi-Fi adapter owns the radio, wifi.conf is ignored
static volatile LONG g_nv, g_nv_scan, g_nv_conn, g_nv_disc, g_nv_pk, g_nv_gk;
static UCHAR g_nv_frame[1100], g_nv_areq[320];
static ULONG g_wl_htcap, g_wl_vhtcap, g_wl_work;
static LONGLONG g_st_rx_us, g_st_poll_us;
static ULONG g_st_rx_n, g_st_loops, g_st_ind, g_st_pre, g_st_mcs, g_st_bw, g_st_sgi, g_st_peak, g_st_qdrop;
static ULONG g_ns_ht, g_ns_vht, g_ns_wmm, g_ns_mode, g_ns_cf1, g_ns_bw, g_ns_htcap, g_ns_ampdu, g_ns_vhtcap, g_ns_vrx, g_ns_vtx, g_ns_noht, g_ns_mbps, g_nr_off;
static ULONG g_nv_fn, g_nv_chan, g_nv_rsn, g_nv_areqn, g_nv_stage, g_nv_connected, g_nv_reason;
static LONGLONG g_nv_down_t, g_nv_pk_t;
static PVOID g_net_cb;
static UCHAR g_ns_ssid[33], g_ns_pass[64], g_ns_pmk[32], g_ns_ptk[48], g_ns_snonce[32], g_ns_rates[16], g_ns_gtk[32], g_ns_bssid[6];
static ULONG g_ns_ssidlen, g_ns_passlen, g_ns_nrates, g_ns_gtklen, g_ns_gtkid, g_ns_cap, g_ns_bint, g_ns_dtim, g_ns_freq, g_ns_rsn, g_ns_aid, g_ns_retry, g_ns_have_conf, g_ns_best = 0xffffffff;
static int g_ns, g_ns_peermap;
static LONGLONG g_ns_t;
static UCHAR *g_nt_buf, *g_nm_buf;
static ULONG g_nt_pa, g_nm_pa, g_nm_slot, g_nm_id, g_nt_busy[NET_TXN], g_nt_inflight, g_nt_last;
static UCHAR g_nq[NET_QN][1516];
static USHORT g_nq_len[NET_QN];
static volatile ULONG g_nq_h, g_nq_t;
static KSPIN_LOCK g_nq_lock;
static ULONG g_nr_ok, g_nr_drop, g_nr_log, g_nt_ok, g_nt_compl;
static UCHAR *g_htt_buf;
static ULONG g_htt_buf_pa;

static const UCHAR g_rsn_ie[22] = { 0x30, 0x14, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 2, 0, 0 };

// ---- SHA-1, HMAC, PBKDF2, AES-128 unwrap (RFC 3394) ----
static ULONG Rol(ULONG x, int n) { return (x << n) | (x >> (32 - n)); }

static void Sha1Block(ULONG *h, const UCHAR *p)
{
    ULONG w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], i, f, k, t;

    for (i = 0; i < 16; i++)
        w[i] = ((ULONG)p[4 * i] << 24) | ((ULONG)p[4 * i + 1] << 16) | ((ULONG)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (; i < 80; i++)
        w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else             { f = b ^ c ^ d; k = 0xca62c1d6; }
        t = Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

// two-part message: enough for HMAC
static void Sha1(const UCHAR *m1, ULONG n1, const UCHAR *m2, ULONG n2, UCHAR *out)
{
    ULONG h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 }, i, f = 0, tot = n1 + n2;
    UCHAR b[64];

    for (i = 0; i < tot; i++) {
        b[f++] = i < n1 ? m1[i] : m2[i - n1];
        if (f == 64) { Sha1Block(h, b); f = 0; }
    }
    b[f++] = 0x80;
    if (f > 56) {
        while (f < 64) b[f++] = 0;
        Sha1Block(h, b);
        f = 0;
    }
    while (f < 56) b[f++] = 0;
    b[56] = b[57] = b[58] = 0;
    b[59] = (UCHAR)(tot >> 29);
    b[60] = (UCHAR)(tot >> 21); b[61] = (UCHAR)(tot >> 13); b[62] = (UCHAR)(tot >> 5); b[63] = (UCHAR)(tot << 3);
    Sha1Block(h, b);
    for (i = 0; i < 5; i++) {
        out[4 * i] = (UCHAR)(h[i] >> 24); out[4 * i + 1] = (UCHAR)(h[i] >> 16); out[4 * i + 2] = (UCHAR)(h[i] >> 8); out[4 * i + 3] = (UCHAR)h[i];
    }
}

static void Hmac(const UCHAR *key, ULONG kn, const UCHAR *m, ULONG n, UCHAR *out)
{
    UCHAR k[64], in[20];
    ULONG i;

    for (i = 0; i < 64; i++) k[i] = (i < kn ? key[i] : 0) ^ 0x36;
    Sha1(k, 64, m, n, in);
    for (i = 0; i < 64; i++) k[i] ^= 0x36 ^ 0x5c;
    Sha1(k, 64, in, 20, out);
}

static void Pbkdf2(const UCHAR *pw, ULONG pn, const UCHAR *salt, ULONG sn, UCHAR *out)
{
    UCHAR b[40], u[20], t[20];
    ULONG blk, i, k, o = 0;

    for (blk = 1; blk <= 2; blk++) {
        for (i = 0; i < sn; i++) b[i] = salt[i];
        b[sn] = 0; b[sn + 1] = 0; b[sn + 2] = 0; b[sn + 3] = (UCHAR)blk;
        Hmac(pw, pn, b, sn + 4, u);
        for (i = 0; i < 20; i++) t[i] = u[i];
        for (k = 1; k < 4096; k++) {
            UCHAR v[20];
            Hmac(pw, pn, u, 20, v);
            for (i = 0; i < 20; i++) { u[i] = v[i]; t[i] ^= v[i]; }
        }
        for (i = 0; i < 20 && o < 32; i++) out[o++] = t[i];
    }
}

static UCHAR g_sb[256], g_isb[256];

static void AesTables(void)
{
    UCHAR p = 1, q = 1;
    int i;

    do {
        p = (UCHAR)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= q << 1; q ^= q << 2; q ^= q << 4;
        if (q & 0x80) q ^= 0x09;
        g_sb[p] = (UCHAR)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)) ^ 0x63);
    } while (p != 1);
    g_sb[0] = 0x63;
    for (i = 0; i < 256; i++) g_isb[g_sb[i]] = (UCHAR)i;
}

static UCHAR Gm(UCHAR a, UCHAR b)
{
    UCHAR r = 0;

    while (b) {
        if (b & 1) r ^= a;
        a = (UCHAR)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b >>= 1;
    }
    return r;
}

static void AesDec(const UCHAR *key, UCHAR *s)
{
    UCHAR rk[176], rc = 1, t[16];
    int r, i;

    for (i = 0; i < 16; i++) rk[i] = key[i];
    for (i = 16; i < 176; i += 4) {
        UCHAR a = rk[i - 4], b = rk[i - 3], c = rk[i - 2], d = rk[i - 1];
        if (i % 16 == 0) {
            UCHAR x = a;
            a = (UCHAR)(g_sb[b] ^ rc); b = g_sb[c]; c = g_sb[d]; d = g_sb[x];
            rc = (UCHAR)((rc << 1) ^ ((rc & 0x80) ? 0x1b : 0));
        }
        rk[i] = rk[i - 16] ^ a; rk[i + 1] = rk[i - 15] ^ b; rk[i + 2] = rk[i - 14] ^ c; rk[i + 3] = rk[i - 13] ^ d;
    }
    for (i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (r = 9; r >= 0; r--) {
        for (i = 0; i < 16; i++) t[(i + 4 * ((i % 4))) % 16] = g_isb[s[i]];     // inverse ShiftRows + SubBytes
        for (i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
        if (r) {
            for (i = 0; i < 16; i += 4) {
                UCHAR a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];
                s[i]     = Gm(a, 14) ^ Gm(b, 11) ^ Gm(c, 13) ^ Gm(d, 9);
                s[i + 1] = Gm(a, 9) ^ Gm(b, 14) ^ Gm(c, 11) ^ Gm(d, 13);
                s[i + 2] = Gm(a, 13) ^ Gm(b, 9) ^ Gm(c, 14) ^ Gm(d, 11);
                s[i + 3] = Gm(a, 11) ^ Gm(b, 13) ^ Gm(c, 9) ^ Gm(d, 14);
            }
        }
    }
}

// RFC 3394 unwrap: in has n + 8 bytes, out receives n bytes; returns 0 when the integrity check passes
static int AesUnwrap(const UCHAR *kek, const UCHAR *in, ULONG n, UCHAR *out)
{
    UCHAR a[8], b[16];
    ULONG i, m = n / 8, t;
    int j;

    for (i = 0; i < 8; i++) a[i] = in[i];
    for (i = 0; i < n; i++) out[i] = in[8 + i];
    for (j = 5; j >= 0; j--) {
        for (i = m; i >= 1; i--) {
            ULONG k;
            t = m * (ULONG)j + i;
            for (k = 0; k < 8; k++) b[k] = a[k];
            b[7] ^= (UCHAR)t; b[6] ^= (UCHAR)(t >> 8);
            for (k = 0; k < 8; k++) b[8 + k] = out[(i - 1) * 8 + k];
            AesDec(kek, b);
            for (k = 0; k < 8; k++) { a[k] = b[k]; out[(i - 1) * 8 + k] = b[8 + k]; }
        }
    }
    for (i = 0; i < 8; i++)
        if (a[i] != 0xa6)
            return -1;
    return 0;
}

// ---- binding to the NDIS miniport and the transmit queue ----
static int NetBusTx(const UCHAR *f, ULONG n)
{
    KIRQL k;
    ULONG i, h;
    int r = -1;

    if ((g_ns != NS_UP && !(g_nv && g_ns == NS_4WAY)) || n > 1514)
        return -1;
    k = KeAcquireSpinLockRaiseToDpc(&g_nq_lock);
    h = g_nq_h;
    if (h - g_nq_t >= NET_QN) g_st_qdrop++;
    if (h - g_nq_t < NET_QN) {
        for (i = 0; i < n; i++)
            g_nq[h % NET_QN][i] = f[i];
        g_nq_len[h % NET_QN] = (USHORT)n;
        g_nq_h = h + 1;
        r = 0;
    }
    KeReleaseSpinLock(&g_nq_lock, k);
    return r;
}

static int NvScan(void) { InterlockedExchange(&g_nv_scan, 1); return 0; }
static void NvDisconnect(void) { InterlockedExchange(&g_nv_disc, 1); }

static int NvConnect(const UCHAR *f, ULONG n, ULONG chan, ULONG rsn)
{
    if (n > sizeof(g_nv_frame) || g_nv_conn)
        return -1;
    WlCopy(g_nv_frame, f, n);
    g_nv_fn = n;
    g_nv_chan = chan;
    g_nv_rsn = rsn;
    InterlockedExchange(&g_nv_conn, 1);
    return 0;
}

// Windows ran the 4-way handshake itself and hands over the keys. Called once for each key; the service thread installs them.
static void NvKey(ULONG idx, ULONG group, const UCHAR *k, ULONG n)
{
    if (n != 16)
        return;
    if (group) {
        WlCopy(g_ns_gtk, k, n);
        g_ns_gtklen = n;
        g_ns_gtkid = idx;
        InterlockedExchange(&g_nv_gk, 1);
    } else {
        WlCopy(g_ns_ptk + 32, k, n);
        g_nv_pk_t = NowUs();
        InterlockedExchange(&g_nv_pk, 1);
    }
}

static void NetCb(PVOID ctx, PVOID a1, PVOID a2)
{
    (void)ctx;
    if (a1 == (PVOID)1 && a2) {
        ((J606F_WIFI_BIND *)a2)->tx = NetBusTx;
        g_nb = (J606F_WIFI_BIND *)a2;
    } else if (a1 == (PVOID)2 && a2) {
        J606F_WIFI_BIND *b = a2;
        b->scan = NvScan;
        b->connect = NvConnect;
        b->disconnect = NvDisconnect;
        b->key = NvKey;
        b->tx = NetBusTx;
        g_nb = b;
        g_nv = 1;
    } else if (a1 == (PVOID)0) {
        g_nv = 0;
        g_nb = NULL;
    }
}

static void NetInit(void)
{
    UNICODE_STRING n;
    OBJECT_ATTRIBUTES oa;
    UCHAR *b;
    ULONG len = 0, i = 0;

    AesTables();
    g_nv_wanted = FileThere(L"\\??\\C:\\j606f\\drv\\wifi.on");
    g_ns_noht = FileThere(L"\\??\\C:\\j606f\\drv\\wifi.legacy");
    Log("wlan: net: native adapter %s", g_nv_wanted ? "expected" : "not expected");
    RtlInitUnicodeString(&n, L"\\Callback\\J606fWlan");
    InitializeObjectAttributes(&oa, &n, OBJ_CASE_INSENSITIVE | 0x10 /* OBJ_PERMANENT */, NULL, NULL);
    if (NT_SUCCESS(ExCreateCallback(&g_net_cb, &oa, TRUE, TRUE)))
        ExRegisterCallback(g_net_cb, NetCb, NULL);
    b = LoadWhole(L"\\??\\C:\\j606f\\drv\\wifi.conf", &len, 512);
    if (!b) {
        Log("wlan: net: no wifi.conf");
        return;
    }
    if (len >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf)
        i = 3;
    while (i < len) {
        ULONG e = i, k;
        while (e < len && b[e] != '\n' && b[e] != '\r') e++;
        if (e - i > 5 && b[i] == 's' && b[i + 1] == 's' && b[i + 2] == 'i' && b[i + 3] == 'd' && b[i + 4] == '=' && e - i - 5 <= 32) {
            g_ns_ssidlen = e - i - 5;
            for (k = 0; k < g_ns_ssidlen; k++) g_ns_ssid[k] = b[i + 5 + k];
        } else if (e - i > 4 && b[i] == 'p' && b[i + 1] == 's' && b[i + 2] == 'k' && b[i + 3] == '=' && e - i - 4 >= 8 && e - i - 4 <= 63) {
            g_ns_passlen = e - i - 4;
            for (k = 0; k < g_ns_passlen; k++) g_ns_pass[k] = b[i + 4 + k];
        }
        i = e + 1;
    }
    for (i = 0; i < len; i++) b[i] = 0;
    ExFreePoolWithTag(b, SVC_TAG);
    g_ns_have_conf = g_ns_ssidlen && g_ns_passlen;
    Log("wlan: net: wifi.conf %s", g_ns_have_conf ? "accepted" : "NOT usable (need ssid= and psk= of 8..63 characters)");
}

static void NetPrep(void)
{
    WlZero(g_nt_busy, sizeof(g_nt_busy));
    g_nt_buf = WlAlloc(NET_TXN * 2048, &g_nt_pa);
    g_nm_buf = WlAlloc(4 * 512, &g_nm_pa);
}

static void WmiStartScan(void);
static void NetState(int s, const char *what)
{
    g_ns = s;
    g_ns_t = NowUs();
    Log("wlan: net: %s (t+%lu s)", what, (unsigned long)((g_ns_t - g_t_live) / 1000000));
}

// ---- WMI commands ----
static void NetPutMac(const UCHAR *m)
{
    UCHAR v[8] = { m[0], m[1], m[2], m[3], m[4], m[5], 0, 0 };

    WmiPut(v, 8);
}

static void NetChannelTlv(void)
{
    ULONG c[6];

    c[0] = g_ns_freq; c[1] = g_ns_cf1 ? g_ns_cf1 : g_ns_freq; c[2] = 0;
    c[3] = g_ns_mode;                                   // enum wmi_phy_mode
    c[4] = (40u << 8) | (40u << 16);
    c[5] = 40u << 8;
    WmiTag(80, 24);
    WmiPut(c, 24);
}

static void NetVdevStart(void)
{
    ULONG c[18];
    ULONG i;

    for (i = 0; i < 18; i++) c[i] = 0;
    c[2] = g_ns_bint;
    c[3] = g_ns_dtim ? g_ns_dtim : 1;
    g_wmi_n = 0;
    WmiTag(0x58, 72);
    WmiPut(c, 72);
    NetChannelTlv();
    WmiTag(18, 0);
    WmiCmd(0x5003, "VDEV_START");
}

static void NetPeerCreate(void)
{
    ULONG z = 0;

    g_wmi_n = 0;
    WmiTag(0x61, 16);
    WmiPut(&z, 4);
    NetPutMac(g_ns_bssid);
    WmiPut(&z, 4);
    WmiCmd(0x6001, "PEER_CREATE");
}

static void NetMgmtSend(const UCHAR *f, ULONG n)
{
    ULONG s = g_nm_slot++ % 4, c[3] = { 0, g_nm_id++, 0 }, pa = g_nm_pa + 512 * s, z = 0, blen = n > 64 ? 64 : n, i;
    UCHAR copy[68];

    WlCopy(g_nm_buf + 512 * s, f, n);
    WlDsb();
    blen = (blen + 3) & ~3u;
    for (i = 0; i < blen; i++) copy[i] = i < n ? f[i] : 0;
    g_wmi_n = 0;
    WmiTag(0x1a6, 28);
    WmiPut(c, 12);
    WmiPut(&pa, 4);
    WmiPut(&z, 4);
    WmiPut(&n, 4);
    WmiPut(&blen, 4);
    WmiTag(17, blen);
    WmiPut(copy, blen);
    WmiCmd(0x7008, "MGMT_TX");
}

static ULONG NetMgmtHdr(UCHAR *f, UCHAR fc)
{
    ULONG i;

    f[0] = fc; f[1] = 0; f[2] = 0; f[3] = 0;
    for (i = 0; i < 6; i++) { f[4 + i] = g_ns_bssid[i]; f[10 + i] = g_wl_mac[i]; f[16 + i] = g_ns_bssid[i]; }
    f[22] = 0; f[23] = 0;
    return 24;
}

static void NetSendAuth(void)
{
    UCHAR f[40];
    ULONG n = NetMgmtHdr(f, 0xb0), i;

    for (i = 0; i < 6; i++) f[n + i] = 0;
    f[n + 2] = 1;                                       // open system, sequence 1, status 0
    NetMgmtSend(f, n + 6);
}

// HT/VHT elements of the association request: the chip's real abilities (SERVICE_READY), one spatial stream, no beamforming.
static ULONG NetPutCaps(UCHAR *f, ULONG n)
{
    ULONG i, v;

    if (g_ns_ht) {
        v = 0x000c | 0x0002 | 0x1000;                   // SM power save off, 20/40 MHz, DSSS/CCK in 40
        if (g_wl_htcap & 0x0002) v |= 0x0020;           // short GI 20
        if (g_wl_htcap & 0x0800) v |= 0x0040;           // short GI 40
        if ((g_wl_htcap & 0x0040) || ((g_wl_htcap & 0x3000) == 0x3000)) v |= 0x0001;    // LDPC
        v |= ((g_wl_htcap & 0x0030) >> 4) << 8;         // RX STBC
        if (g_wl_vhtcap & 3) v |= 0x0800;               // long A-MSDU
        f[n++] = 45; f[n++] = 26;
        f[n++] = (UCHAR)v; f[n++] = (UCHAR)(v >> 8);
        f[n++] = 0x1b;                                  // A-MPDU 64 KB, spacing 8 us (as ath10k)
        for (i = 0; i < 16; i++) f[n + i] = 0;
        f[n] = 0xff;                                    // MCS 0-7
        f[n + 12] = 1;                                  // TX MCS set defined
        n += 16;
        for (i = 0; i < 7; i++) f[n++] = 0;
    }
    if (g_ns_vht) {
        v = g_wl_vhtcap & (0x3u | 0x10u | 0x20u | 0x700u | 0x3800000u | 0x30000000u);   // no 160 MHz, no beamforming
        f[n++] = 191; f[n++] = 12;
        for (i = 0; i < 4; i++) f[n++] = (UCHAR)(v >> (8 * i));
        f[n++] = 0xfe; f[n++] = 0xff; f[n++] = 0; f[n++] = 0;   // RX: stream 1 MCS 0-9
        f[n++] = 0xfe; f[n++] = 0xff; f[n++] = 0; f[n++] = 0;   // TX: stream 1 MCS 0-9
    }
    if (g_ns_wmm) {
        static const UCHAR w[9] = { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };
        for (i = 0; i < 9; i++) f[n++] = w[i];
    }
    return n;
}

static void NetSendAssoc(void)
{
    UCHAR f[300];
    ULONG n = NetMgmtHdr(f, 0x00), i, cap = g_ns_cap & 0x0431, nb = g_ns_nrates > 8 ? 8 : g_ns_nrates;

    f[n++] = (UCHAR)cap; f[n++] = (UCHAR)(cap >> 8);
    f[n++] = 10; f[n++] = 0;                            // listen interval
    f[n++] = 0; f[n++] = (UCHAR)g_ns_ssidlen;
    for (i = 0; i < g_ns_ssidlen; i++) f[n++] = g_ns_ssid[i];
    f[n++] = 1; f[n++] = (UCHAR)nb;
    for (i = 0; i < nb; i++) f[n++] = g_ns_rates[i];
    if (g_ns_nrates > 8) {
        f[n++] = 50; f[n++] = (UCHAR)(g_ns_nrates - 8);
        for (i = 8; i < g_ns_nrates; i++) f[n++] = g_ns_rates[i];
    }
    if (g_ns_rsn)
        for (i = 0; i < sizeof(g_rsn_ie); i++) f[n++] = g_rsn_ie[i];
    n = NetPutCaps(f, n);
    WlCopy(g_nv_areq, f, n);
    g_nv_areqn = n;
    NetMgmtSend(f, n);
}

static ULONG NetDensity(ULONG d)
{
    static const UCHAR m[8] = { 0, 1, 1, 1, 2, 4, 8, 16 };      // ath10k_parse_mpdudensity

    return m[d & 7];
}

static void NetPeerAssoc(void)
{
    ULONG c[19], i, n = 0, blen, nht = 0, vr[4] = { 0, 0, 0, 0 };
    UCHAR r[16], h[8];

    for (i = 0; i < g_ns_nrates && n < 16; i++) {
        UCHAR v = g_ns_rates[i] & 0x7f;
        r[n++] = (v == 2 || v == 4 || v == 11 || v == 22) ? (UCHAR)(v | 0x80) : v;
    }
    blen = (n + 3) & ~3u;
    for (i = n; i < blen; i++) r[i] = 0;
    for (i = 0; i < 19; i++) c[i] = 0;
    c[1] = 1;                       // new_assoc
    c[2] = g_ns_aid;
    c[3] = g_ns_rsn ? (1 | 4) : 1;  // AUTH | NEED_PTK_4_WAY
    if (g_ns_wmm) c[3] |= 2;        // QOS
    c[4] = g_ns_cap;
    c[5] = 10;                      // listen interval
    c[10] = 1;                      // nss
    c[12] = g_ns_mode;
    if (g_ns_ht) {
        ULONG cap = g_ns_htcap;
        if (!(g_wl_htcap & 0x0002)) cap &= ~0x0020u;
        if (!(g_wl_htcap & 0x0800)) cap &= ~0x0040u;
        cap &= 0x0001 | 0x0002 | 0x000c | 0x0020 | 0x0040 | 0x0800 | 0x1000;    // no STBC toward a 1-chain radio
        c[3] |= 0x1000;             // HT
        c[6] = cap;
        c[7] = (1u << (13 + (g_ns_ampdu & 3))) - 1;
        c[8] = NetDensity((g_ns_ampdu >> 2) & 7);
        c[9] = 0x08;                // WMI_RC_HT_FLAG
        if (cap & 0x0001) c[3] |= 0x10000;      // LDPC
        if (cap & 0x0060) c[9] |= 0x04;         // short GI
        if (g_ns_bw >= 40) { c[3] |= 0x2000; c[9] |= 0x02; }
        for (i = 0; i < 8; i++) h[i] = (UCHAR)i;
        nht = 8;
    }
    if (g_ns_vht) {
        ULONG e = (g_ns_vhtcap >> 23) & 7, m = (1u << (13 + e)) - 1;
        c[3] |= 0x02000000;         // VHT
        if (g_ns_bw >= 80) c[3] |= 0x04000000;
        c[11] = g_ns_vhtcap & g_wl_vhtcap & (0x3u | 0x10u | 0x20u | 0x3800000u);
        c[11] |= g_ns_vhtcap & 0x3800000u;
        if (m > c[7]) c[7] = m;
        vr[1] = g_ns_vrx;
        vr[3] = g_ns_vtx;
    }
    c[15] = n;                      // num_legacy_rates
    c[16] = nht;                    // num_ht_rates
    g_wmi_n = 0;
    WmiTag(0x65, 76);
    NetPutMac(g_ns_bssid);
    WmiPut(c, 68);
    WmiTag(17, blen);
    WmiPut(r, blen);
    WmiTag(17, nht);
    if (nht) WmiPut(h, nht);
    WmiTag(0x66, 16);
    WmiPut(vr, 16);
    WmiCmd(0x6005, "PEER_ASSOC");
}

static void NetVdevUp(void)
{
    ULONG v[2] = { 0, g_ns_aid };

    g_wmi_n = 0;
    WmiTag(0x5c, 16);
    WmiPut(v, 8);
    NetPutMac(g_ns_bssid);
    WmiCmd(0x5005, "VDEV_UP");
    {
        ULONG ps[2] = { 0, 0 };                         // vdev 0, WMI_STA_PS_MODE_DISABLED: the radio stays awake while connected
        g_wmi_n = 0;
        WmiTag(0x6c, 8);
        WmiPut(ps, 8);
        WmiCmd(0x9001, "STA_POWERSAVE_MODE off");
    }
}

static void NetInstallKey(ULONG idx, ULONG flags, const UCHAR *key, ULONG klen)
{
    UCHAR c[92];
    ULONG i, v;

    WlZero(c, sizeof(c));
    for (i = 0; i < 6; i++) c[4 + i] = g_ns_bssid[i];
    v = idx; WlCopy(c + 12, &v, 4);
    v = flags; WlCopy(c + 16, &v, 4);
    v = NET_CIPHER_CCMP; WlCopy(c + 20, &v, 4);
    v = klen; WlCopy(c + 80, &v, 4);
    g_wmi_n = 0;
    WmiTag(0x60, 92);
    WmiPut(c, 92);
    WmiTag(17, (klen + 3) & ~3u);
    WmiPut(key, klen);
    for (i = klen; i & 3; i++) { UCHAR z = 0; WmiPut(&z, 1); }
    WmiCmd(0x5009, "VDEV_INSTALL_KEY");
}

static void NetAuthorize(void)
{
    ULONG z = 0, v[2] = { 3, 1 };

    g_wmi_n = 0;
    WmiTag(0x64, 20);
    WmiPut(&z, 4);
    NetPutMac(g_ns_bssid);
    WmiPut(v, 8);
    WmiCmd(0x6004, "PEER_SET_PARAM authorize");
}

// ---- HTT data ----
static int NetTx(const UCHAR *f, ULONG n)
{
    UCHAR m[20 + 52];
    volatile UCHAR *d;
    ULONG id = 0xffff, i, k, pa, prefetch = n > 50 ? 50 : n, tid = 16, flags1;

    if (!(f[0] & 1) && g_ns_wmm && n >= 14)             // unicast QoS data: only QoS TIDs are aggregated by the firmware
        tid = (f[12] == 0x88 && f[13] == 0x8e) ? 7 : 0;
    flags1 = (tid << 6) | (1u << 11);

    for (i = 0; i < NET_TXN; i++) {
        k = (g_nt_last + i) % NET_TXN;
        if (!g_nt_busy[k]) { id = k; break; }
    }
    if (id == 0xffff)
        return -1;
    pa = g_nt_pa + 2048 * id;
    WlCopy(g_nt_buf + 2048 * id, f, n);
    d = (volatile UCHAR *)g_htt_frag + 72 * id;
    WlZero(d, 72);
    WlCopy(d + 24, &pa, 4);
    d[30] = (UCHAR)n; d[31] = (UCHAR)(n >> 8);
    WlDsb();
    m[0] = 1;                                           // HTT_H2T TX_FRM
    m[1] = 2u << 5;                                     // packet type Ethernet
    m[2] = (UCHAR)flags1; m[3] = (UCHAR)(flags1 >> 8);
    m[4] = (UCHAR)n; m[5] = (UCHAR)(n >> 8);
    m[6] = (UCHAR)id; m[7] = (UCHAR)(id >> 8);
    k = g_htt_frag_pa + 72 * id;
    WlCopy(m + 8, &k, 4);
    m[12] = m[13] = m[14] = m[15] = 0;
    m[16] = 0xff; m[17] = 0xff; m[18] = 0; m[19] = 0;   // HTT_INVALID_PEERID
    prefetch = (prefetch + 3) & ~3u;
    if (prefetch > n) prefetch = n;
    for (i = 0; i < prefetch; i++) m[20 + i] = f[i];
    if (HtcSend(&g_ce4, (UCHAR)g_wl_htt_ep, 0, m, 20 + prefetch))
        return -1;
    g_nt_busy[id] = 1;
    g_nt_inflight++;
    if (g_nt_inflight > g_st_peak) g_st_peak = g_nt_inflight;
    g_nt_last = id + 1;
    g_nt_ok++;
    return 0;
}

static void NetTxCompl(const UCHAR *p, ULONG n)
{
    ULONG cnt, i;

    if (n < 4)
        return;
    cnt = p[2];
    for (i = 0; i < cnt && 4 + 2 * i + 2 <= n; i++) {
        ULONG id = p[4 + 2 * i] | ((ULONG)p[5 + 2 * i] << 8);
        if (id < NET_TXN && g_nt_busy[id]) {
            g_nt_busy[id] = 0;
            g_nt_inflight--;
            g_nt_compl++;
        }
    }
}

// ---- WPA2-PSK 4-way handshake ----
static void NetDeriveKeys(const UCHAR *anonce)
{
    static const char lab[] = "Pairwise key expansion";
    UCHAR d[23 + 76 + 1], o[60];
    const UCHAR *a = g_wl_mac, *b = g_ns_bssid, *x = g_ns_snonce, *y = anonce;
    ULONG i, n = 0;
    int cmp = 0;

    for (i = 0; i < 6 && !cmp; i++) cmp = (int)a[i] - (int)b[i];
    if (cmp > 0) { a = g_ns_bssid; b = g_wl_mac; }
    cmp = 0;
    for (i = 0; i < 32 && !cmp; i++) cmp = (int)x[i] - (int)y[i];
    if (cmp > 0) { x = anonce; y = g_ns_snonce; }
    for (i = 0; i < 22; i++) d[n++] = (UCHAR)lab[i];
    d[n++] = 0;
    for (i = 0; i < 6; i++) d[n++] = a[i];
    for (i = 0; i < 6; i++) d[n++] = b[i];
    for (i = 0; i < 32; i++) d[n++] = x[i];
    for (i = 0; i < 32; i++) d[n++] = y[i];
    for (i = 0; i < 3; i++) {
        d[n] = (UCHAR)i;
        Hmac(g_ns_pmk, 32, d, n + 1, o + 20 * i);
    }
    for (i = 0; i < 48; i++) g_ns_ptk[i] = o[i];
    WlZero(o, sizeof(o));
}

static void NetEapolSend(UCHAR *e, ULONG len, int mic)
{
    UCHAR f[14 + 300], m[20];
    ULONG i;

    if (mic) {
        for (i = 0; i < 16; i++) e[81 + i] = 0;
        Hmac(g_ns_ptk, 16, e, len, m);
        for (i = 0; i < 16; i++) e[81 + i] = m[i];
    }
    for (i = 0; i < 6; i++) { f[i] = g_ns_bssid[i]; f[6 + i] = g_wl_mac[i]; }
    f[12] = 0x88; f[13] = 0x8e;
    for (i = 0; i < len; i++) f[14 + i] = e[i];
    i = NetTx(f, 14 + len);
    if (i)
        Log("wlan: net: EAPOL tx failed");
}

static void NetEapolReply(const UCHAR *in, ULONG info, const UCHAR *nonce, const UCHAR *data, ULONG dlen)
{
    UCHAR e[300];
    ULONG i;

    WlZero(e, 99 + dlen);
    e[0] = in[0]; e[1] = 3;
    e[2] = (UCHAR)((95 + dlen) >> 8); e[3] = (UCHAR)(95 + dlen);
    e[4] = 2;
    e[5] = (UCHAR)(info >> 8); e[6] = (UCHAR)info;
    e[7] = in[7]; e[8] = in[8];
    for (i = 0; i < 8; i++) e[9 + i] = in[9 + i];
    if (nonce)
        for (i = 0; i < 32; i++) e[17 + i] = nonce[i];
    e[97] = (UCHAR)(dlen >> 8); e[98] = (UCHAR)dlen;
    for (i = 0; i < dlen; i++) e[99 + i] = data[i];
    NetEapolSend(e, 99 + dlen, 1);
}

static int NetMicOk(UCHAR *e, ULONG len)
{
    UCHAR saved[16], m[20];
    ULONG i;
    int bad = 0;

    for (i = 0; i < 16; i++) { saved[i] = e[81 + i]; e[81 + i] = 0; }
    Hmac(g_ns_ptk, 16, e, len, m);
    for (i = 0; i < 16; i++) { bad |= saved[i] ^ m[i]; e[81 + i] = saved[i]; }
    return !bad;
}

static void NetGtkFrom(const UCHAR *d, ULONG n)
{
    ULONG o = 0, i;

    while (o + 2 <= n) {
        ULONG l = d[o + 1];
        if (d[o] == 0xdd && l >= 6 && o + 2 + l <= n && d[o + 2] == 0 && d[o + 3] == 0x0f && d[o + 4] == 0xac && d[o + 5] == 1) {
            g_ns_gtkid = d[o + 6] & 3;
            g_ns_gtklen = l - 6;
            if (g_ns_gtklen > sizeof(g_ns_gtk)) g_ns_gtklen = sizeof(g_ns_gtk);
            for (i = 0; i < g_ns_gtklen; i++) g_ns_gtk[i] = d[o + 8 + i];
            return;
        }
        if (d[o] == 0xdd && l == 0)
            return;
        o += 2 + l;
    }
}

static void NetEapol(const UCHAR *pkt, ULONG n)
{
    static UCHAR e[1600], w[300];
    ULONG ki, dlen, i, ver;

    if (n < 99 || n > sizeof(e))
        return;
    for (i = 0; i < n; i++) e[i] = pkt[i];
    if (e[1] != 3 || e[4] != 2)
        return;
    ki = ((ULONG)e[5] << 8) | e[6];
    dlen = ((ULONG)e[97] << 8) | e[98];
    ver = ki & 7;
    if (99 + dlen > n)
        return;
    if (ver != 2) {
        Log("wlan: net: EAPOL key version %lu not supported", (unsigned long)ver);
        return;
    }
    if ((ki & 8) && (ki & 0x80) && !(ki & 0x100)) {     // message 1
        UCHAR h[20], sd[32 + 16];
        LONGLONG t = NowUs();
        for (i = 0; i < 32; i++) sd[i] = g_ns_pmk[i];
        WlCopy(sd + 32, &t, 8);
        WlCopy(sd + 40, e + 17, 8);
        Sha1(sd, 48, NULL, 0, h);
        for (i = 0; i < 20; i++) g_ns_snonce[i] = h[i];
        sd[0] ^= 1;
        Sha1(sd, 48, NULL, 0, h);
        for (i = 0; i < 12; i++) g_ns_snonce[20 + i] = h[i];
        NetDeriveKeys(e + 17);
        NetEapolReply(e, 0x010a, g_ns_snonce, g_rsn_ie, sizeof(g_rsn_ie));
        Log("wlan: net: 4-way 1/4 received, 2/4 sent");
    } else if ((ki & 8) && (ki & 0x100) && (ki & 0x80) && (ki & 0x40)) {     // message 3
        if (!NetMicOk(e, 99 + dlen)) {
            Log("wlan: net: 4-way 3/4 MIC mismatch (wrong password?)");
            return;
        }
        g_ns_gtklen = 0;
        if ((ki & 0x1000) && dlen >= 16 && dlen - 8 <= sizeof(w) && !AesUnwrap(g_ns_ptk + 16, e + 99, dlen - 8, w))
            NetGtkFrom(w, dlen - 8);
        WlZero(w, sizeof(w));
        Log("wlan: net: 4-way 3/4 ok, group key %s", g_ns_gtklen ? "present" : "absent");
        NetEapolReply(e, 0x030a, NULL, NULL, 0);
        NetState(NS_WAITM4, "4-way 4/4 sent");
    } else if (!(ki & 8) && (ki & 0x100) && (ki & 0x80) && NetMicOk(e, 99 + dlen)) {     // group key handshake
        if ((ki & 0x1000) && dlen >= 16 && dlen - 8 <= sizeof(w) && !AesUnwrap(g_ns_ptk + 16, e + 99, dlen - 8, w)) {
            NetGtkFrom(w, dlen - 8);
            WlZero(w, sizeof(w));
            NetEapolReply(e, 0x0302, NULL, NULL, 0);
            if (g_ns_gtklen)
                NetInstallKey(g_ns_gtkid, 1, g_ns_gtk, g_ns_gtklen);
            Log("wlan: net: group key updated");
        }
    }
}

// ---- receive path ----
static void NetRxFrame(const UCHAR *d, ULONG n)
{
    ULONG type = n >= 14 ? ((ULONG)d[12] << 8) | d[13] : 0;

    if (g_nr_log < 12) {
        g_nr_log++;
        Log("wlan: net: rx frame %lu B, ethertype %04lx", (unsigned long)n, (unsigned long)type);
    }
    if (g_nv) {                         // Windows handles EAPOL: everything goes up once the peer is associated
        if (g_nb && n >= 14 && (type == 0x888e || g_ns == NS_UP || g_ns == NS_4WAY)) {
            LONGLONG t0 = NowUs();
            g_nb->rx(g_nb->ctx, d, n);
            g_st_rx_us += NowUs() - t0;
            g_st_rx_n++;
            g_nr_ok++;
        } else {
            g_nr_drop++;
        }
    } else if (type == 0x888e) {
        if (n > 14)
            NetEapol(d + 14, n - 14);
    } else if (g_ns == NS_UP && g_nb && n >= 14) {
        g_nb->rx(g_nb->ctx, d, n);
        g_nr_ok++;
    } else {
        g_nr_drop++;
    }
}

static void NetInOrd(const UCHAR *p, ULONG n)
{
    ULONG cnt, o = 8, i;
    int skip;

    if (n < 8 || !g_htt_buf)
        return;
    g_st_ind++;
    skip = (p[1] & 0x60) != 0;                          // offloaded or fragmented MSDUs are not handled yet
    if (skip) g_nr_off++;
    cnt = p[6] | ((ULONG)p[7] << 8);
    for (i = 0; i < cnt && o + 12 <= n; i++, o += 12) {
        ULONG pa = Le32p(p + o), len = p[o + 8] | ((ULONG)p[o + 9] << 8), k;
        if (pa < g_htt_buf_pa || pa >= g_htt_buf_pa + HTT_RX_N * HTT_RX_BUF)
            continue;
        {
            volatile ULONG *rd = (volatile ULONG *)(g_htt_buf + (pa - g_htt_buf_pa));
            ULONG i1 = rd[32], pre = i1 >> 24;          // rx_ppdu_start: valid on the first MPDU of a PPDU only
            if (g_nv && g_nb && (pre == 4 || pre == 8 || pre == 9 || pre == 12 || pre == 13) && (rd[31] & 0xff) < 0x80)
                g_nb->rssi = (LONG)(rd[31] & 0xff) - 95;
            if (pre == 8 || pre == 9) {
                g_st_pre = pre; g_st_mcs = rd[33] & 0x1f; g_st_bw = 20u << ((rd[33] >> 7) & 1); g_st_sgi = (rd[34] >> 7) & 1;
            } else if (pre == 12 || pre == 13) {
                g_st_pre = pre; g_st_mcs = (rd[34] >> 4) & 15; g_st_bw = 20u << (rd[33] & 3); g_st_sgi = rd[34] & 1;
            } else if (pre == 4) {
                g_st_pre = pre; g_st_mcs = (i1 >> 0) & 15; g_st_bw = 20; g_st_sgi = 0;
            }
        }
        if (!skip && len >= 14 && len <= HTT_RX_BUF - 360)
            NetRxFrame(g_htt_buf + (pa - g_htt_buf_pa) + 360, len);
        else
            g_nr_drop++;
        *(volatile ULONG *)(g_htt_buf + (pa - g_htt_buf_pa) + 4) = 0;   // clear the attention word before posting again
        k = *g_htt_idx;
        g_htt_ring[2 * k] = pa;
        g_htt_ring[2 * k + 1] = 0;
        WlDsb();
        *g_htt_idx = (k + 1) & (HTT_RX_N - 1);
        WlDsb();
    }
}

// ---- management frames and the connection state machine ----
static void NetBeacon(const UCHAR *f, ULONG buf, ULONG chan, ULONG snr)
{
    ULONG o = 36, match = 0, rsn = 0, nr = 0, dtim = 0, i;
    UCHAR rates[16];

    while (o + 2 <= buf && o + 2 + f[o + 1] <= buf) {
        ULONG l = f[o + 1], id = f[o];
        if (id == 0 && l == g_ns_ssidlen) {
            for (i = 0; i < l && f[o + 2 + i] == g_ns_ssid[i]; i++)
                ;
            match = i == l;
        } else if (id == 48) {
            rsn = 1;
        } else if (id == 5 && l >= 2) {
            dtim = f[o + 3];
        } else if ((id == 1 || id == 50)) {
            for (i = 0; i < l && nr < 16; i++)
                rates[nr++] = f[o + 2 + i];
        }
        o += 2 + l;
    }
    if (!match || (g_ns_best != 0xffffffff && snr <= g_ns_best))
        return;
    g_ns_best = snr;
    for (i = 0; i < 6; i++) g_ns_bssid[i] = f[16 + i];
    g_ns_freq = chan == 14 ? 2484 : chan < 14 ? 2407 + 5 * chan : 5000 + 5 * chan;
    g_ns_bint = f[32] | ((ULONG)f[33] << 8);
    g_ns_cap = f[34] | ((ULONG)f[35] << 8);
    g_ns_dtim = dtim;
    g_ns_rsn = rsn;
    g_ns_nrates = nr;
    for (i = 0; i < nr; i++) g_ns_rates[i] = rates[i];
    g_ns_ht = g_ns_vht = g_ns_wmm = 0;                  // the wifi.conf path stays legacy
    g_ns_mode = g_ns_freq < 3000 ? 1 : 0;
    g_ns_cf1 = g_ns_freq;
    g_ns_bw = 20;
    g_ns_mbps = 54;
}

// returns nonzero when the frame was consumed by the connection logic
static int NetMgmtRx(const UCHAR *f, ULONG buf, ULONG chan, ULONG snr)
{
    ULONG i;

    if (buf < 24)
        return 0;
    if ((f[0] == 0x80 || f[0] == 0x50) && buf >= 36 && g_nv && g_nb)
        g_nb->bss(g_nb->ctx, f, buf, chan, (LONG)snr - 95);
    if ((f[0] == 0x80 || f[0] == 0x50) && buf >= 36 && g_ns_have_conf && g_ns == NS_OFF && !g_nv)
        NetBeacon(f, buf, chan, snr);
    if (f[0] != 0xb0 && f[0] != 0x10 && f[0] != 0xc0 && f[0] != 0xa0)
        return 0;
    for (i = 0; i < 6; i++)
        if (f[10 + i] != g_ns_bssid[i])
            return 1;
    if (f[0] == 0xb0 && g_ns == NS_AUTH && buf >= 30) {
        ULONG seq = f[26] | ((ULONG)f[27] << 8), st = f[28] | ((ULONG)f[29] << 8);
        Log("wlan: net: auth response seq %lu status %lu", (unsigned long)seq, (unsigned long)st);
        if (seq == 2 && st == 0) {
            NetState(NS_ASSOC, "authenticated, sending association request");
            g_ns_retry = 0;
            NetSendAssoc();
        }
    } else if (f[0] == 0x10 && g_ns == NS_ASSOC && buf >= 30) {
        ULONG st = f[26] | ((ULONG)f[27] << 8);
        Log("wlan: net: association response status %lu", (unsigned long)st);
        if (st == 0) {
            g_ns_aid = (f[28] | ((ULONG)f[29] << 8)) & 0x3fff;
            NetPeerAssoc();
            NetVdevUp();
            g_nv_stage = 3;
            NetState(NS_4WAY, "associated, waiting for the 4-way handshake");
            if (g_nv && g_nb) {
                g_nv_connected = 1;
                g_nb->mbps = g_ns_mbps;
                g_nb->assoc(g_nb->ctx, 0, g_nv_areq + 24, g_nv_areqn - 24, f + 24, buf - 24);
                if (!g_ns_rsn) {
                    NetAuthorize();
                    NetState(NS_UP, "CONNECTED, open network");
                }
            }
        } else {
            NetState(NS_FAIL, "association refused");
        }
    } else if (f[0] == 0xc0 || f[0] == 0xa0) {
        g_nv_reason = buf >= 26 ? f[24] | ((ULONG)f[25] << 8) : 0;
        Log("wlan: net: %s reason %lu", f[0] == 0xc0 ? "deauthenticated" : "disassociated", (unsigned long)g_nv_reason);
        if (g_ns == NS_UP && g_nb && !g_nv)
            g_nb->link(g_nb->ctx, 0);
        NetState(NS_FAIL, "connection lost");
    }
    return 1;
}

static void NetBegin(void)
{
    if (g_nv && g_nb) {
        g_nb->scan_done(g_nb->ctx);
        return;
    }
    if (g_nv_wanted || !g_ns_have_conf || g_ns != NS_OFF)
        return;
    if (g_ns_best == 0xffffffff) {
        Log("wlan: net: configured network not found in the scan");
        return;
    }
    if (!g_ns_rsn) {
        Log("wlan: net: configured network is not WPA2, not connecting");
        return;
    }
    Log("wlan: net: network found, channel freq %lu MHz, snr %lu, %lu rates", (unsigned long)g_ns_freq, (unsigned long)g_ns_best, (unsigned long)g_ns_nrates);
    Pbkdf2(g_ns_pass, g_ns_passlen, g_ns_ssid, g_ns_ssidlen, g_ns_pmk);
    WlZero(g_ns_pass, sizeof(g_ns_pass));
    NetState(NS_VSTART, "starting vdev");
    NetVdevStart();
}

static void NetVdevStarted(ULONG status)
{
    Log("wlan: net: VDEV_START response status %lu", (unsigned long)status);
    if (g_ns != NS_VSTART)
        return;
    if (status) {
        NetState(NS_FAIL, "vdev start failed");
        return;
    }
    g_ns_peermap = 0;
    g_nv_stage = 1;
    NetState(NS_PEER, "creating peer");
    NetPeerCreate();
    g_nv_stage = 2;
}

// What the chosen network and this chip can both do: PHY mode, channel width, centre frequency.
static void NetCaps(const UCHAR *f, ULONG buf)
{
    static const UCHAR c80[7] = { 42, 58, 106, 122, 138, 155, 171 };
    ULONG o = 36, htop = 0, sec = 0, vop = 0, havevop = 0, ch, i, sgi, wide;

    g_ns_ht = g_ns_vht = g_ns_wmm = 0;
    g_ns_mode = g_ns_freq < 3000 ? 1 : 0;
    g_ns_cf1 = g_ns_freq;
    g_ns_bw = 20;
    g_ns_mbps = 54;
    while (o + 2 <= buf && o + 2 + f[o + 1] <= buf) {
        ULONG l = f[o + 1], id = f[o];
        const UCHAR *e = f + o + 2;
        if (id == 45 && l >= 26) {
            g_ns_ht = 1;
            g_ns_htcap = e[0] | ((ULONG)e[1] << 8);
            g_ns_ampdu = e[2];
        } else if (id == 61 && l >= 22) {
            htop = e[1];
            sec = e[1] & 3;
        } else if (id == 191 && l >= 12) {
            g_ns_vht = 1;
            g_ns_vhtcap = Le32p(e);
            g_ns_vrx = e[4] | ((ULONG)e[5] << 8);
            g_ns_vtx = e[8] | ((ULONG)e[9] << 8);
        } else if (id == 192 && l >= 5) {
            vop = e[0];
            havevop = 1;
        } else if (id == 221 && l >= 7 && e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xf2 && e[3] == 2) {
            g_ns_wmm = 1;
        }
        o += 2 + l;
    }
    if (g_ns_noht || !(g_wl_htcap & 1) || !g_ns_ht) {
        g_ns_ht = g_ns_vht = 0;
    } else {
        if (g_ns_freq < 3000 || !g_wl_vhtcap)
            g_ns_vht = 0;
        wide = (htop & 4) && (sec == 1 || sec == 3) && (g_ns_htcap & 2);
        if (wide) {
            g_ns_bw = 40;
            g_ns_cf1 = sec == 1 ? g_ns_freq + 10 : g_ns_freq - 10;
        }
        if (g_ns_freq < 3000) {
            g_ns_mode = wide ? 7 : 5;                   // 11NG HT40 / HT20
        } else if (g_ns_vht) {
            g_ns_mode = wide ? 9 : 8;                   // 11AC VHT40 / VHT20
            ch = (g_ns_freq - 5000) / 5;
            if (havevop && vop >= 1)
                for (i = 0; i < 7; i++)
                    if (ch + 6 >= c80[i] && ch <= c80[i] + 6u) {
                        g_ns_mode = 10;                 // 11AC VHT80
                        g_ns_bw = 80;
                        g_ns_cf1 = 5000 + 5u * c80[i];
                        break;
                    }
        } else {
            g_ns_mode = wide ? 6 : 4;                   // 11NA HT40 / HT20
        }
        sgi = g_ns_vht && g_ns_bw == 80 ? (g_ns_vhtcap & g_wl_vhtcap & 0x20) != 0 : (g_ns_bw >= 40 ? (g_ns_htcap & 0x40) != 0 && (g_wl_htcap & 0x0800) : (g_ns_htcap & 0x20) != 0 && (g_wl_htcap & 2));
        if (g_ns_vht) {
            ULONG m = g_ns_vrx & 3;                     // highest MCS of the first stream: 0 = MCS 7, 1 = MCS 8, 2 = MCS 9
            static const USHORT t[3][3][2] = { { { 65, 72 }, { 78, 86 }, { 78, 86 } }, { { 135, 150 }, { 162, 180 }, { 180, 200 } }, { { 292, 325 }, { 351, 390 }, { 390, 433 } } };
            if (m > 2) m = 0;
            g_ns_mbps = t[g_ns_bw == 80 ? 2 : g_ns_bw == 40 ? 1 : 0][m][sgi ? 1 : 0];
        } else if (g_ns_ht) {
            g_ns_mbps = g_ns_bw >= 40 ? (sgi ? 150 : 135) : (sgi ? 72 : 65);
        }
    }
    Log("wlan: net: phy mode %lu, %lu MHz, centre %lu, ht %lu vht %lu wmm %lu, link %lu Mbit/s, chip ht %lx vht %lx", (unsigned long)g_ns_mode, (unsigned long)g_ns_bw,
        (unsigned long)g_ns_cf1, (unsigned long)g_ns_ht, (unsigned long)g_ns_vht, (unsigned long)g_ns_wmm, (unsigned long)g_ns_mbps, (unsigned long)g_wl_htcap, (unsigned long)g_wl_vhtcap);
}

static void NvParse(const UCHAR *f, ULONG buf, ULONG chan)
{
    ULONG o = 36, nr = 0, dtim = 0, i;

    g_ns_ssidlen = 0;
    while (o + 2 <= buf && o + 2 + f[o + 1] <= buf) {
        ULONG l = f[o + 1], id = f[o];
        if (id == 0 && l <= 32) {
            g_ns_ssidlen = l;
            for (i = 0; i < l; i++) g_ns_ssid[i] = f[o + 2 + i];
        } else if (id == 5 && l >= 2) {
            dtim = f[o + 3];
        } else if (id == 1 || id == 50) {
            for (i = 0; i < l && nr < 16; i++)
                g_ns_rates[nr++] = f[o + 2 + i];
        }
        o += 2 + l;
    }
    for (i = 0; i < 6; i++) g_ns_bssid[i] = f[16 + i];
    g_ns_freq = chan == 14 ? 2484 : chan < 14 ? 2407 + 5 * chan : 5000 + 5 * chan;
    g_ns_bint = f[32] | ((ULONG)f[33] << 8);
    g_ns_cap = f[34] | ((ULONG)f[35] << 8);
    g_ns_dtim = dtim;
    g_ns_nrates = nr;
    g_ns_rsn = g_nv_rsn;
    NetCaps(f, buf);
}

static void NvCmd(ULONG tag, ULONG id, const char *name, int with_mac)
{
    ULONG z = 0;

    g_wmi_n = 0;
    WmiTag(tag, with_mac ? 12 : 4);
    WmiPut(&z, 4);
    if (with_mac)
        NetPutMac(g_ns_bssid);
    WmiCmd(id, name);
}

// Leave the network: firmware objects are removed in the reverse order of creation, then the radio is free for the next scan or connection.
static void NvTeardown(int report)
{
    if (g_nv_stage >= 3)
        NvCmd(0x5e, 0x5007, "VDEV_DOWN", 0);
    if (g_nv_stage >= 2)
        NvCmd(0x62, 0x6002, "PEER_DELETE", 1);
    if (g_nv_stage >= 1)
        NvCmd(0x5d, 0x5006, "VDEV_STOP", 0);
    g_nv_stage = 0;
    g_nq_t = g_nq_h;
    g_nv_pk = g_nv_gk = 0;
    g_nv_down_t = NowUs();
    if (report && g_nb) {
        if (g_nv_connected)
            g_nb->lost(g_nb->ctx, g_nv_reason);
        else
            g_nb->assoc(g_nb->ctx, 1, NULL, 0, NULL, 0);
    }
    g_nv_connected = 0;
    NetState(NS_OFF, "idle");
}

static void NvService(void)
{
    if (InterlockedExchange(&g_nv_disc, 0) && g_ns != NS_OFF)
        NvTeardown(0);
    if (g_ns == NS_FAIL)
        NvTeardown(1);
    if (g_nv_scan) {
        if (g_ns != NS_OFF || !g_nb) {
            g_nv_scan = 0;
            if (g_nb)
                g_nb->scan_done(g_nb->ctx);
        } else if (g_wl_scan_done) {
            g_nv_scan = 0;
            g_wl_scan_done = 0;
            WmiStartScan();
        }
    }
    if (g_nv_conn && g_ns == NS_OFF && g_wl_scan_done && NowUs() - g_nv_down_t > 300000) {
        NvParse(g_nv_frame, g_nv_fn, g_nv_chan);
        g_nv_conn = 0;
        g_nv_reason = 0;
        NetState(NS_VSTART, "windows connect: starting vdev");
        NetVdevStart();
    }
    if (g_ns == NS_4WAY && g_nv_pk && (!g_nt_inflight || NowUs() - g_nv_pk_t > 100000)) {
        g_nv_pk = 0;
        NetInstallKey(0, 0, g_ns_ptk + 32, 16);
        NetAuthorize();
        NetState(NS_UP, "CONNECTED, keys from windows installed");
    }
    if (g_ns == NS_UP && g_nv_pk) {         // rekey
        g_nv_pk = 0;
        NetInstallKey(0, 0, g_ns_ptk + 32, 16);
    }
    if (g_ns == NS_UP && g_nv_gk) {
        g_nv_gk = 0;
        NetInstallKey(g_ns_gtkid, 1, g_ns_gtk, g_ns_gtklen);
    }
}

static void NetService(void)
{
    LONGLONG dt;
    int budget;

    if (g_nv)
        NvService();
    if (g_ns == NS_OFF || g_ns == NS_FAIL)
        return;
    dt = NowUs() - g_ns_t;
    if (g_ns == NS_PEER && (g_ns_peermap || dt > 1500000)) {
        NetState(NS_AUTH, g_ns_peermap ? "peer mapped, authenticating" : "no peer map event, authenticating anyway");
        g_ns_retry = 0;
        NetSendAuth();
    } else if (g_ns == NS_AUTH && dt > 1000000) {
        if (g_ns_retry++ < 3) { g_ns_t = NowUs(); NetSendAuth(); Log("wlan: net: auth retry"); }
        else NetState(NS_FAIL, "no auth response");
    } else if (g_ns == NS_ASSOC && dt > 1000000) {
        if (g_ns_retry++ < 3) { g_ns_t = NowUs(); NetSendAssoc(); Log("wlan: net: assoc retry"); }
        else NetState(NS_FAIL, "no association response");
    } else if (g_ns == NS_VSTART && dt > 3000000) {
        NetState(NS_FAIL, "no VDEV_START response");
    } else if (g_ns == NS_4WAY && dt > 15000000 && !g_nv) {
        NetState(NS_FAIL, "4-way handshake timed out");
    } else if (g_ns == NS_WAITM4 && (dt > 100000 || !g_nt_inflight)) {
        NetInstallKey(0, 0, g_ns_ptk + 32, 16);
        if (g_ns_gtklen)
            NetInstallKey(g_ns_gtkid, 1, g_ns_gtk, g_ns_gtklen);
        NetAuthorize();
        NetState(NS_UP, "CONNECTED, keys installed");
        if (g_nb)
            g_nb->link(g_nb->ctx, 1);
        else
            Log("wlan: net: no NDIS adapter bound");
    }
    if (g_ns == NS_UP || (g_nv && g_ns == NS_4WAY)) {
        for (budget = 0; budget < 64 && g_nq_t != g_nq_h; budget++) {
            ULONG k = g_nq_t % NET_QN;
            if (NetTx(g_nq[k], g_nq_len[k]))
                break;
            g_nq_t++;
            g_wl_work++;
        }
    }
    {
        static LONGLONG lt, lr, lp;
        static ULONG ln, ll, li;
        LONGLONG now = NowUs();
        if (now - lt > 2000000 && g_st_rx_n - ln > 2000) {
            ULONG dn = g_st_rx_n - ln;
            Log("wlan: stat %lu ms: rx %lu frames, %lu indications, rx call %lu us, poll %lu ms, loops %lu; rx preamble %lu mcs %lu bw %lu sgi %lu; tx peak %lu qdrop %lu", (unsigned long)((now - lt) / 1000), (unsigned long)dn, (unsigned long)(g_st_ind - li),
                (unsigned long)((g_st_rx_us - lr) / dn), (unsigned long)((g_st_poll_us - lp) / 1000), (unsigned long)(g_st_loops - ll), (unsigned long)g_st_pre, (unsigned long)g_st_mcs, (unsigned long)g_st_bw, (unsigned long)g_st_sgi, (unsigned long)g_st_peak, (unsigned long)g_st_qdrop);
            g_st_peak = 0;
        }
        if (now - lt > 2000000) {
            lt = now; lr = g_st_rx_us; lp = g_st_poll_us; ln = g_st_rx_n; ll = g_st_loops; li = g_st_ind;
        }
    }
    {
        static LONGLONG last;
        if (g_ns == NS_UP && NowUs() - last > 30000000) {
            last = NowUs();
            Log("wlan: net: offloaded %lu; tx %lu done %lu inflight %lu, rx ok %lu dropped %lu", (unsigned long)g_nr_off, (unsigned long)g_nt_ok, (unsigned long)g_nt_compl, (unsigned long)g_nt_inflight,
                (unsigned long)g_nr_ok, (unsigned long)g_nr_drop);
        }
    }
}
