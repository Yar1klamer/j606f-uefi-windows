// J606F Wi-Fi step F2: copy engines + HTC (ath10k ce.c, snoc.c, htc.c for WCN3990), polled from the btbus thread after WLAN_MODE.
//  DMA: the SMMU is on and faults unmatched streams, so the WLAN stream (0x1a0 mask 1, as in the DT) gets its own stage-1 context bank whose page
//  tables identity-map ONLY the driver's 3 MiB DMA pool. No bypass: everything else stays unreachable for the device.
//  Only CE0/CE3 (host->target) and CE1/CE2 (target->host) are set up. Log lines start with "wlan:".
#define WL_MEM_PA       0x0c800000u
#define WL_CE0_OFF      0x00240000u
#define WL_SHADOW_OFF   0x00032000u
#define WL_SMMU_PA      0x0c600000u
#define WL_SMMU_SIZE    0x00080000u
#define WL_STREAM       0x1a0u
#define WL_STREAM_MASK  0x1u
#define WL_POOL_SIZE    0x00400000u
#define WL_BUF          2048

typedef struct { int ce; volatile UCHAR *desc; ULONG desc_pa, n, mask, sw, wr; UCHAR *buf; ULONG buf_pa; } CEPIPE;

static volatile UCHAR *g_wl_ce, *g_wl_smmu, *g_wl_cb;
static UCHAR *g_wl_pool;
static ULONG g_wl_pool_pa, g_wl_pool_off, g_wl_cbn;
static int g_wl_on, g_wl_state, g_wl_rxlog;
static CEPIPE g_ce0, g_ce1, g_ce2, g_ce3, g_ce4, g_ce5, g_ce8, g_ce9, g_ce10, g_ce11;
static LONGLONG g_wl_t;
static ULONG g_wl_credits, g_wl_credit_size, g_wl_wmi_ep = 0xff, g_wl_htt_ep = 0xff, g_wl_ep_credits[8], g_wl_rx_n;
static UCHAR g_wl_seq[8];

#define WL_CE(n)        (g_wl_ce + WL_CE0_OFF + 0x1000 * (n))

static void WlDsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void WlZero(volatile void *d, ULONG n)
{
    volatile UCHAR *p = (volatile UCHAR *)d;

    while (n--)
        *p++ = 0;
}

static void WlCopy(volatile void *d, const void *s, ULONG n)
{
    volatile UCHAR *p = (volatile UCHAR *)d;
    const UCHAR *q = (const UCHAR *)s;

    while (n && ((ULONG_PTR)p & 7)) {                   // the pool is non-cached: 8-byte aligned writes are far cheaper than bytes
        *p++ = *q++;
        n--;
    }
    for (; n >= 8; n -= 8, p += 8, q += 8)
        *(volatile unsigned long long *)p = *(const unsigned long long *)q;   // the source is cached memory: unaligned reads are legal
    while (n--)
        *p++ = *q++;
}

static UCHAR *WlAlloc(ULONG size, ULONG *pa)
{
    UCHAR *p = g_wl_pool + g_wl_pool_off;

    *pa = g_wl_pool_pa + g_wl_pool_off;
    g_wl_pool_off += (size + 15) & ~15u;
    return p;
}

static int WlSmmu(void)
{
    PHYSICAL_ADDRESS lo, hi, bnd;
    U64 *t;
    ULONG tpa, i, n, ncb, smr = 0xffff, cb = 0xffff, a;
    int used;

    lo.QuadPart = 0; hi.QuadPart = 0xffffffffLL; bnd.QuadPart = 0;
    t = (U64 *)MmAllocateContiguousMemorySpecifyCache(6 * 4096, lo, hi, bnd, MmNonCached);
    if (!t) { Log("wlan: no memory for page tables"); return -1; }
    WlZero(t, 6 * 4096);
    tpa = (ULONG)MmGetPhysicalAddress(t).QuadPart;
    if ((g_wl_pool_pa >> 30) != ((g_wl_pool_pa + WL_POOL_SIZE - 1) >> 30)) { Log("wlan: pool crosses 1 GiB"); return -1; }
    {                                                   // L1 (4 entries, T0SZ 32) -> L2 -> up to four L3 tables; NC, RW, AF, nG, outer shareable
        ULONG first = g_wl_pool_pa >> 21;
        U64 *l2 = t + 512;
        t[g_wl_pool_pa >> 30] = (U64)(tpa + 4096) | 3;
        for (a = g_wl_pool_pa; a < g_wl_pool_pa + WL_POOL_SIZE; a += 4096) {
            ULONG k = (a >> 21) - first;
            l2[(a >> 21) & 511] = (U64)(tpa + (2 + k) * 4096) | 3;
            t[1024 + 512 * k + ((a >> 12) & 511)] = (U64)a | 0xe43;
        }
    }
    WlDsb();
    g_wl_smmu = Map(WL_SMMU_PA, WL_SMMU_SIZE);
    if (!g_wl_smmu) { Log("wlan: smmu map failed"); return -1; }
    n = Rd(g_wl_smmu, 0x20) & 0xff;
    ncb = Rd(g_wl_smmu, 0x24) & 0xff;
    for (i = 0; i < n && i < 128; i++)
        if (!(Rd(g_wl_smmu, 0x800 + 4 * i) & 0x80000000)) { smr = i; break; }
    for (i = ncb; i-- > 0 && cb == 0xffff; ) {          // highest context bank that is off and that no valid stream uses
        ULONG k;
        used = (Rd(g_wl_smmu, 0x40000 + 0x1000 * i) & 1) != 0;
        for (k = 0; !used && k < n && k < 128; k++)
            if ((Rd(g_wl_smmu, 0x800 + 4 * k) & 0x80000000) && (Rd(g_wl_smmu, 0xc00 + 4 * k) & 0xff) == i)
                used = 1;
        if (!used)
            cb = i;
    }
    Log("wlan: smmu free smr %lu, free cb %lu of %lu, tables %08lx, pool %08lx", (unsigned long)smr, (unsigned long)cb, (unsigned long)ncb,
        (unsigned long)tpa, (unsigned long)g_wl_pool_pa);
    if (smr == 0xffff || cb == 0xffff)
        return -1;
    g_wl_cbn = cb;
    g_wl_cb = g_wl_smmu + 0x40000 + 0x1000 * cb;
    Wr(g_wl_smmu, 0x1000 + 4 * cb, (1u << 16) | (0xfu << 12) | (3u << 8));      // CBAR: S1 translate, S2 bypass
    Wr(g_wl_smmu, 0x1800 + 4 * cb, 1);                  // CBA2R: VA64
    Wr(g_wl_cb, 0x10, (7u << 15) | 1);                  // TCR2: SEP upstream, PASIZE 36 bit
    Wr(g_wl_cb, 0x30, (1u << 23) | (2u << 12) | 32);    // TCR: EPD1, SH outer, walks non-cacheable, 4 KiB granule, T0SZ 32
    Wr(g_wl_cb, 0x20, tpa);                             // TTBR0 (ASID = context bank number)
    Wr(g_wl_cb, 0x24, cb << 16);
    Wr(g_wl_cb, 0x38, 0x44);                            // MAIR0: attr0 = normal non-cacheable
    Wr(g_wl_cb, 0x58, 0xffffffff);                      // clear FSR
    WlDsb();
    Wr(g_wl_cb, 0, (1u << 5) | (1u << 2) | (1u << 1) | 1);   // SCTLR: CFRE, AFE, TRE, M
    Wr(g_wl_cb, 0x610, cb);                             // TLBIASID
    Wr(g_wl_cb, 0x7f0, 0);
    for (i = 0; i < 1000 && (Rd(g_wl_cb, 0x7f4) & 1); i++)
        KeStallExecutionProcessor(10);
    Wr(g_wl_smmu, 0xc00 + 4 * smr, cb);                 // S2CR: translate via this context bank
    WlDsb();
    Wr(g_wl_smmu, 0x800 + 4 * smr, 0x80000000u | (WL_STREAM_MASK << 16) | WL_STREAM);
    WlDsb();
    Log("wlan: smmu smr[%lu] %08lx s2cr %08lx cbar %08lx sctlr %08lx tcr %08lx ttbr0 %08lx gfsr %08lx", (unsigned long)smr,
        (unsigned long)Rd(g_wl_smmu, 0x800 + 4 * smr), (unsigned long)Rd(g_wl_smmu, 0xc00 + 4 * smr), (unsigned long)Rd(g_wl_smmu, 0x1000 + 4 * cb),
        (unsigned long)Rd(g_wl_cb, 0), (unsigned long)Rd(g_wl_cb, 0x30), (unsigned long)Rd(g_wl_cb, 0x20), (unsigned long)Rd(g_wl_smmu, 0x48));
    return 0;
}

static void CeInitSrc(CEPIPE *p, int ce, ULONG n)
{
    volatile UCHAR *c = WL_CE(ce);

    p->ce = ce; p->n = n; p->mask = n - 1;
    p->desc = WlAlloc(16 * n, &p->desc_pa);
    p->buf = WlAlloc(WL_BUF * n, &p->buf_pa);
    p->sw = Rd(c, 0x44) & p->mask;
    p->wr = Rd(c, 0x3c) & p->mask;
    Wr(c, 0x00, p->desc_pa);
    Wr(c, 0x04, Rd(c, 0x04) & ~0x1fu);
    Wr(c, 0x08, n);
    Wr(c, 0x18, (Rd(c, 0x18) & ~(0xffffu | (1u << 17))) | WL_BUF);       // DMAX, no byte swap
    Log("wlan: ce%d src %lu entries at %08lx, sw %lu wr %lu ctrl %08lx", ce, (unsigned long)n, (unsigned long)p->desc_pa, (unsigned long)p->sw,
        (unsigned long)p->wr, (unsigned long)Rd(c, 0x18));
}

static void CeInitDst(CEPIPE *p, int ce, ULONG n)
{
    volatile UCHAR *c = WL_CE(ce);
    ULONG i;

    p->ce = ce; p->n = n; p->mask = n - 1;
    p->desc = WlAlloc(16 * n, &p->desc_pa);
    p->buf = WlAlloc(WL_BUF * n, &p->buf_pa);
    p->sw = Rd(c, 0x48) & p->mask;
    p->wr = Rd(c, 0x40) & p->mask;
    Wr(c, 0x0c, p->desc_pa);
    Wr(c, 0x10, Rd(c, 0x10) & ~0x1fu);
    Wr(c, 0x14, n);
    Wr(c, 0x18, Rd(c, 0x18) & ~(1u << 18));
    for (i = 0; i < n; i++) {
        volatile ULONG *d = (volatile ULONG *)(p->desc + 16 * i);
        d[0] = p->buf_pa + WL_BUF * i;
        d[1] = 0;
        d[2] = 0;
        d[3] = 0;
    }
    p->wr = (p->sw + n - 1) & p->mask;                  // all but one slot posted
    WlDsb();
    Wr(c, 0x40, p->wr);
    Log("wlan: ce%d dst %lu entries at %08lx, sw %lu wr %lu", ce, (unsigned long)n, (unsigned long)p->desc_pa, (unsigned long)p->sw, (unsigned long)p->wr);
}

static int CeSend(CEPIPE *p, const void *data, ULONG len, UCHAR eid)
{
    volatile ULONG *d;
    ULONG next = (p->wr + 1) & p->mask, rd = Rd(WL_CE(p->ce), 0x44) & p->mask;

    if (next == rd || len > WL_BUF)
        return -1;
    WlCopy(p->buf + WL_BUF * p->wr, data, len);
    d = (volatile ULONG *)(p->desc + 16 * p->wr);
    d[0] = p->buf_pa + WL_BUF * p->wr;
    d[1] = 0;
    d[2] = len | ((ULONG)eid << (16 + 4));              // nbytes | flags (meta data = transfer id = endpoint)
    d[3] = 0;
    p->wr = next;
    WlDsb();
    Wr(g_wl_ce, WL_SHADOW_OFF + 4 * p->ce, p->wr);      // source write index goes through the shadow register
    return 0;
}

static int HtcSend(CEPIPE *p, UCHAR eid, UCHAR flags, const void *pl, ULONG plen)
{
    UCHAR f[WL_BUF];
    ULONG i;

    if (plen + 8 > sizeof(f))
        return -1;
    f[0] = eid; f[1] = flags; f[2] = (UCHAR)plen; f[3] = (UCHAR)(plen >> 8);
    f[4] = 0; f[5] = g_wl_seq[eid & 7]++; f[6] = 0; f[7] = 0;
    for (i = 0; i < plen; i++)
        f[8 + i] = ((const UCHAR *)pl)[i];
    return CeSend(p, f, plen + 8, eid);
}

static void HtcConnect(USHORT svc, USHORT flags)
{
    UCHAR m[8] = { 2, 0, (UCHAR)svc, (UCHAR)(svc >> 8), (UCHAR)flags, (UCHAR)(flags >> 8), 0, 0 };
    int r = HtcSend(&g_ce0, 0, 0, m, 8);

    Log("wlan: -> HTC CONNECT service %04x flags %04x: %d", svc, flags, r);
}

static void WlHex(const char *tag, const UCHAR *d, ULONG n)
{
    static const char hx[] = "0123456789abcdef";
    char line[160];
    ULONG i, k = 0;

    for (i = 0; i < n && i < 40 && k + 4 < sizeof(line); i++) {
        line[k++] = ' ';
        line[k++] = hx[d[i] >> 4];
        line[k++] = hx[d[i] & 15];
    }
    line[k] = 0;
    Log("wlan: %s (%lu B):%s", tag, (unsigned long)n, line);
}

// ---- WMI TLV: bring-up + passive scan (ath10k wmi-tlv.c). SSIDs and BSSIDs are never written to the log: only counts, channel, level, SSID length. ----
#define WQ_N 8
static UCHAR g_wq[WQ_N][2000];
static ULONG g_wq_len[WQ_N], g_wq_h, g_wq_t;
static UCHAR g_wmi[1960];
static ULONG g_wmi_n;
static UCHAR g_wl_mac[6], g_wl_bss[64][6];
static ULONG g_wl_rd, g_wl_nbss, g_wl_mgmt;
static int g_wl_scan_done;
static ULONG g_wl_hid[24], g_wl_hn[24];
static LONGLONG g_wl_init_at;
static UCHAR g_wl_memcopy[640];
static ULONG g_wl_memlen, g_wl_reorder;

static void WmiPut(const void *v, ULONG n)
{
    ULONG i;

    for (i = 0; i < n && g_wmi_n < sizeof(g_wmi); i++)
        g_wmi[g_wmi_n++] = ((const UCHAR *)v)[i];
}

static void WmiTag(USHORT tag, ULONG len)
{
    UCHAR h[4] = { (UCHAR)len, (UCHAR)(len >> 8), (UCHAR)tag, (UCHAR)(tag >> 8) };

    WmiPut(h, 4);
}

static void WmiCmd(ULONG cmd, const char *name)
{
    ULONG k = g_wq_h % WQ_N, i;
    UCHAR *q = g_wq[k];

    if (g_wq_h - g_wq_t >= WQ_N) {
        Log("wlan: WMI queue full, %s dropped", name);
        return;
    }
    q[0] = (UCHAR)cmd; q[1] = (UCHAR)(cmd >> 8); q[2] = (UCHAR)(cmd >> 16); q[3] = 0;
    for (i = 0; i < g_wmi_n; i++)
        q[4 + i] = g_wmi[i];
    g_wq_len[k] = 4 + g_wmi_n;
    g_wq_h++;
    Log("wlan: -> WMI %s (%lu B)", name, (unsigned long)(4 + g_wmi_n));
}

static void WmiFlush(void)
{
    if (g_wl_wmi_ep >= 8)
        return;
    while (g_wq_t != g_wq_h && g_wl_ep_credits[g_wl_wmi_ep]) {
        ULONG k = g_wq_t % WQ_N;
        if (HtcSend(&g_ce3, (UCHAR)g_wl_wmi_ep, 1, g_wq[k], g_wq_len[k]))
            return;
        g_wl_ep_credits[g_wl_wmi_ep]--;
        g_wq_t++;
    }
}

// find TLV `tag` (first one after offset *from, when given)
static const UCHAR *WmiTlvFind(const UCHAR *p, ULONG n, USHORT tag, ULONG *len)
{
    ULONG o = 0;

    while (o + 4 <= n) {
        ULONG l = p[o] | (p[o + 1] << 8);
        USHORT t = p[o + 2] | (p[o + 3] << 8);
        if (o + 4 + l > n)
            return NULL;
        if (t == tag) {
            *len = l;
            return p + o + 4;
        }
        o += 4 + l;
    }
    return NULL;
}

static void WmiInit(const UCHAR *ev, const UCHAR *mem, ULONG memlen, ULONG num_req, ULONG full_reorder)
{
    static const ULONG tmpl[44] = {
        4, 33, 0, 0, 2, 66, 16, 7, 7, 0x64, 0x64, 0x64, 0x28, 2 /* Ethernet */, 4, 4, 4, 8, 0, 0, 0, 0x400, 2, 0, 0, 0, 0, 2,
        1056, 2, 1, 0x20, 2, 5, 22, 6, 0, 1, 1, 0, 0, 0, 0, 1u << 9,
    };
    ULONG cfg[44], i, chunks = 0, o = 0, chunk[16][4];

    for (i = 0; i < 44; i++)
        cfg[i] = tmpl[i];
    if (full_reorder)
        cfg[2] = cfg[3] = 4;
    while (mem && o + 20 <= memlen && chunks < 16) {     // wlan_host_mem_req TLVs
        ULONG id = Le32p(mem + o + 4), unit = Le32p(mem + o + 8), info = Le32p(mem + o + 12), num = Le32p(mem + o + 16), size, pa;
        Log("wlan: host mem request id %lu unit %lu info %lx units %lu", (unsigned long)id, (unsigned long)unit, (unsigned long)info, (unsigned long)num);
        if (!num)
            num = (info & 3) == 1 ? 33 : (info & 3) == 2 ? 4 : 1;
        size = unit * num;
        g_wl_pool_off = (g_wl_pool_off + 4095) & ~4095u;
        if (g_wl_pool_off + size > WL_POOL_SIZE) {
            Log("wlan: host mem request does not fit the pool (%lu B)", (unsigned long)size);
            g_wl_state = -1;
            return;
        }
        WlAlloc(size, &pa);
        chunk[chunks][0] = id; chunk[chunks][1] = pa; chunk[chunks][2] = size; chunk[chunks][3] = 0;
        chunks++;
        o += 20;
    }
    g_wmi_n = 0;
    WmiTag(74, 28);
    {
        static const ULONG abi[6] = { 0x01000000, 53, 0x5f414351, 0x00004c4d, 0, 0 };   // the ABI this client implements (not the firmware's own, 878)
        (void)ev;
        WmiPut(abi, 24);
    }
    WmiPut(&chunks, 4);
    WmiTag(75, sizeof(cfg));
    WmiPut(cfg, sizeof(cfg));
    WmiTag(18, 20 * chunks);
    for (i = 0; i < chunks; i++) {
        WmiTag(76, 16);
        WmiPut(chunk[i], 16);
    }
    (void)num_req;
    WmiCmd(0x1, "INIT");
}

#define HTT_RX_N        1024u
#define HTT_RX_BUF      2048u
#define HTT_DESCS       1056u
#define NET_CIPHER_CCMP 4u                       // WMI_TLV_CIPHER_AES_CCM
static volatile ULONG *g_htt_ring, *g_htt_idx;
static ULONG g_htt_ring_pa, g_htt_idx_pa, g_htt_frag_pa, g_htt_state, g_htt_hist[32];
static UCHAR *g_htt_frag;
static LONGLONG g_htt_t, g_wl_scan_at;
#include "wlnet.inc"

static void WmiStartScan(void)
{
    static const USHORT g5[] = { 5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700,
                                 5745, 5765, 5785, 5805, 5825 };
    ULONG nch = 13 + sizeof(g5) / sizeof(g5[0]), i, mhz;
    ULONG rd[6] = { 0, g_wl_rd, g_wl_rd, g_wl_rd, 0, 0 };
    static int once;

    if (!once) {                                        // a repeat scan only needs START_SCAN
    once = 1;
    g_wmi_n = 0;                                        // regulatory domain from the device's own calibration
    WmiTag(81, sizeof(rd));
    WmiPut(rd, sizeof(rd));
    WmiCmd(0x4001, "PDEV_SET_REGDOMAIN");

    g_wmi_n = 0;                                        // every channel passive: the radio only listens
    WmiTag(79, 4);
    WmiPut(&nch, 4);
    WmiTag(18, nch * 28);
    for (i = 0; i < nch; i++) {
        ULONG c[6];
        mhz = i < 13 ? 2412 + 5 * i : g5[i - 13];
        c[0] = mhz;
        c[1] = mhz;
        c[2] = 0;
        c[3] = (i < 13 ? 1 : 0) | (1u << 7) | (1u << 11) | (1u << 12);   // MODE_11G / MODE_11A | PASSIVE | ALLOW_HT | ALLOW_VHT
        c[4] = (40u << 8) | (40u << 16);
        c[5] = 40u << 8;
        WmiTag(80, 24);
        WmiPut(c, 24);
    }
    WmiCmd(0x3003, "SCAN_CHAN_LIST");

    g_wmi_n = 0;
    if (g_nb)                                           // the adapter's own address, the one Windows uses as the source of its frames
        for (i = 0; i < 6; i++)
            g_wl_mac[i] = g_nb->mac[i];
    {
        ULONG v[5] = { 0, 2, 0, 0, 0 };                 // vdev 0, STA, no subtype, MAC follows
        UCHAR m[8] = { 0 };
        for (i = 0; i < 6; i++)
            m[i] = g_wl_mac[i];
        WmiTag(86, 20);
        WmiPut(v, 12);
        WmiPut(m, 8);
    }
    WmiCmd(0x5001, "VDEV_CREATE");

    g_wmi_n = 0;
    {
        ULONG v[3] = { 0, 0x30, 2 };                    // tx_encap_type = Ethernet
        WmiTag(0x5f, 12);
        WmiPut(v, 12);
    }
    WmiCmd(0x5008, "VDEV_SET_PARAM tx_encap");
    }

    g_wmi_n = 0;
    {
        ULONG c[21] = { 0xa000, 0xa001, 0, 1, 0x10f | 0x10, 50, 150, 50, 500, 0, 0, 0, 20000, 5, 0x31, 0, nch, 1, 0, 0, 3 };
        UCHAR zero[16] = { 0 }, ff[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0 };
        WmiTag(77, sizeof(c) + 16);
        WmiPut(c, sizeof(c));
        WmiPut(zero, 16);
        WmiTag(16, nch * 4);
        for (i = 0; i < nch; i++) {
            mhz = i < 13 ? 2412 + 5 * i : g5[i - 13];
            WmiPut(&mhz, 4);
        }
        WmiTag(19, 0);                                  // no SSIDs
        WmiTag(19, 8);
        WmiPut(ff, 8);                                  // broadcast BSSID
        WmiTag(17, 0);                                  // no IEs
    }
    WmiCmd(0x3001, "START_SCAN (passive)");
}

// ---- HTT setup (ath10k_htt_setup, wcn3990 64-bit layouts). Buffers are posted but not processed yet: management frames come over WMI. ----

static void HttPut16(UCHAR *m, ULONG o, ULONG v) { m[o] = (UCHAR)v; m[o + 1] = (UCHAR)(v >> 8); }
static void HttPut32(UCHAR *m, ULONG o, ULONG v) { HttPut16(m, o, v & 0xffff); HttPut16(m, o + 2, v >> 16); }

static int HttSend(const UCHAR *m, ULONG n, const char *name)
{
    int r = HtcSend(&g_ce4, (UCHAR)g_wl_htt_ep, 0, m, n);

    Log("wlan: -> HTT %s (%lu B): %d", name, (unsigned long)n, r);
    return r;
}

static void HttStart(void)
{
    static const UCHAR ver[4] = { 0, 0, 0, 0 };
    ULONG need = 16 * 3 + HTT_RX_N * 8 + HTT_RX_N * HTT_RX_BUF + HTT_DESCS * 72 + NET_TXN * 2048 + 4 * 512 + 64, i, buf_pa;

    if (g_wl_htt_ep >= 8 || g_wl_pool_off + need > WL_POOL_SIZE) {
        Log("wlan: HTT not started: ep %lu, pool used %lu, need %lu", (unsigned long)g_wl_htt_ep, (unsigned long)g_wl_pool_off, (unsigned long)need);
        g_wl_scan_at = NowUs();
        return;
    }
    g_htt_idx = (volatile ULONG *)WlAlloc(16, &g_htt_idx_pa);
    g_htt_ring = (volatile ULONG *)WlAlloc(HTT_RX_N * 8, &g_htt_ring_pa);
    g_htt_buf = WlAlloc(HTT_RX_N * HTT_RX_BUF, &buf_pa);
    g_htt_buf_pa = buf_pa;
    g_htt_frag = WlAlloc(HTT_DESCS * 72, &g_htt_frag_pa);
    NetPrep();
    for (i = 0; i < HTT_RX_N - 1; i++) {                // as the fill level for dual MAC targets: ring size - 1
        g_htt_ring[2 * i] = buf_pa + HTT_RX_BUF * i;
        g_htt_ring[2 * i + 1] = 0;
    }
    WlDsb();
    *g_htt_idx = HTT_RX_N - 1;
    WlDsb();
    Log("wlan: HTT rx ring %lu entries, %lu posted, pool used %lu of %lu", (unsigned long)HTT_RX_N, (unsigned long)*g_htt_idx, (unsigned long)g_wl_pool_off,
        (unsigned long)WL_POOL_SIZE);
    g_htt_state = 1;
    g_htt_t = NowUs();
    HttSend(ver, 4, "VERSION_REQ");
}

static void HttConfig(void)
{
    static const USHORT off[10] = { 74, 90, 27, 37, 4, 26, 7, 12, 1, 2 };       // offsetof(struct htt_rx_desc_v2, ...) / 4, in htt_rx_ring_rx_desc_offsets order
    UCHAR m[64];
    ULONG i;
    int r;

    WlZero(m, sizeof(m));                               // FRAG_DESC_BANK_CFG (64-bit): info 0, 1 bank, 72-byte descriptors, no queue state
    m[0] = 6;
    m[2] = 1;
    m[3] = 72;
    HttPut32(m, 4, g_htt_frag_pa);
    HttPut16(m, 36 + 2, HTT_DESCS - 1);
    m[60] = 1;                                          // q_state.record_size
    r = HttSend(m, 64, "FRAG_DESC_BANK_CFG");

    WlZero(m, sizeof(m));                               // RX_RING_CFG (64-bit), one ring
    m[0] = 2;
    m[1] = 1;
    HttPut32(m, 4, g_htt_idx_pa);
    HttPut32(m, 12, g_htt_ring_pa);
    HttPut16(m, 20, HTT_RX_N);
    HttPut16(m, 22, HTT_RX_BUF);
    HttPut16(m, 24, 0xffff);
    HttPut16(m, 26, HTT_RX_N - 1);
    for (i = 0; i < 10; i++)
        HttPut16(m, 28 + 2 * i, off[i]);
    r |= HttSend(m, 48, "RX_RING_CFG");

    WlZero(m, 4);                                       // AGGR_CFG v2
    m[0] = 5;
    m[1] = 64;
    m[2] = 3;
    r |= HttSend(m, 4, "AGGR_CFG");
    g_htt_state = r ? 3 : 2;
    g_wl_scan_at = NowUs() + 200000;
}

static void HttRx(const UCHAR *p, ULONG n)
{
    ULONG t;

    if (!n)
        return;
    t = p[0];
    if (t < 32 && g_htt_hist[t]++ < 3)
        Log("wlan: <- HTT type %lu, %lu B", (unsigned long)t, (unsigned long)n);
    if (t == 0 && n >= 4 && g_htt_state == 1) {
        Log("wlan: <- HTT VERSION_CONF %u.%u", p[2], p[1]);
        HttConfig();
    } else if (t == 0x12) {
        NetInOrd(p, n);
    } else if (t == 7) {
        NetTxCompl(p, n);
    } else if (t == 3) {
        g_ns_peermap = 1;
    }
}

static void WmiMgmt(const UCHAR *p, ULONG n)
{
    ULONG l, fl, i, k;
    const UCHAR *ev = WmiTlvFind(p, n, 44, &l), *f = ev ? WmiTlvFind(p, n, 17, &fl) : NULL;
    ULONG chan, snr, buf;

    g_wl_mgmt++;
    if (!ev || !f || l < 24)
        return;
    chan = Le32p(ev);
    snr = Le32p(ev + 4);
    buf = Le32p(ev + 16);
    if (buf > fl)
        buf = fl;
    if (NetMgmtRx(f, buf, chan, snr))
        return;
    if (buf < 36 || (f[0] != 0x80 && f[0] != 0x50))
        return;
    for (k = 0; k < g_wl_nbss; k++) {
        for (i = 0; i < 6 && g_wl_bss[k][i] == f[16 + i]; i++)
            ;
        if (i == 6)
            return;
    }
    if (g_wl_nbss < 64) {
        ULONG o = 36, ssid = 0;
        for (i = 0; i < 6; i++)
            g_wl_bss[g_wl_nbss][i] = f[16 + i];
        g_wl_nbss++;
        while (o + 2 <= buf) {
            if (f[o] == 0) {
                ssid = f[o + 1];
                break;
            }
            o += 2 + f[o + 1];
        }
        Log("wlan: network #%lu: channel %lu snr %lu, ssid length %lu", (unsigned long)g_wl_nbss, (unsigned long)chan, (unsigned long)snr, (unsigned long)ssid);
    }
}

static void WlWmiRx(const UCHAR *p, ULONG n)
{
    ULONG id, l;
    const UCHAR *ev;

    if (n < 4)
        return;
    id = Le32p(p) & 0xffffff;
    p += 4;
    n -= 4;
    {
        ULONG h;
        for (h = 0; h < 24; h++) {
            if (g_wl_hid[h] == id || !g_wl_hid[h]) {
                g_wl_hid[h] = id;
                g_wl_hn[h]++;
                break;
            }
        }
        if (id == 0x7001 && g_wl_hn[h < 24 ? h : 0] <= 3)
            WlHex("mgmt hdr", p, n > 40 ? 40 : n);     // TLV header + rx metadata only, never the frame (BSSID/SSID stay out of the log)
        if (id == 0x4002 && g_wl_hn[h < 24 ? h : 0] <= 4)
            WlHex("chan info", p, n);
    }
    if (id == 1 && (ev = WmiTlvFind(p, n, 32, &l)) != NULL && l >= 76) {
        ULONG rl, sl = 0, ml = 0, bit65 = 0;
        const UCHAR *reg = WmiTlvFind(p, n, 33, &rl), *svc = WmiTlvFind(p, n, 16, &sl), *mem = WmiTlvFind(p, n, 18, &ml);
        ULONG nreq = Le32p(ev + 72);
        g_wl_rd = (reg && rl >= 4) ? Le32p(reg) : 0;
        g_wl_htcap = Le32p(ev + 40);
        g_wl_vhtcap = Le32p(ev + 44);
        if (svc && sl >= 12)
            bit65 = (Le32p(svc + 8) >> 1) & 1;
        Log("wlan: <- WMI SERVICE_READY: rf chains %lu, tx power %lu..%lu, regdomain %lx, host mem requests %lu, full reorder %lu", (unsigned long)Le32p(ev + 36),
            (unsigned long)Le32p(ev + 52), (unsigned long)Le32p(ev + 56), (unsigned long)g_wl_rd, (unsigned long)nreq, (unsigned long)bit65);
        g_wl_memlen = 0;
        if (mem && ml <= sizeof(g_wl_memcopy)) {
            ULONG q;
            for (q = 0; q < ml; q++)
                g_wl_memcopy[q] = mem[q];
            g_wl_memlen = ml;
        }
        g_wl_reorder = bit65;
        g_wl_init_at = NowUs() + 500000;
    } else if (id == 2 && (ev = WmiTlvFind(p, n, 35, &l)) != NULL && l >= 36) {
        ULONG i, status = Le32p(ev + 32);
        for (i = 0; i < 6; i++)
            g_wl_mac[i] = ev[24 + i];
        Log("wlan: <- WMI READY: status %lu", (unsigned long)status);
        if (status == 0) {
            g_wl_state = 4;
            HttStart();
        } else {
            g_wl_state = -1;
        }
    } else if (id == 0x3001 && (ev = WmiTlvFind(p, n, 36, &l)) != NULL && l >= 12) {
        ULONG type = Le32p(ev), reason = Le32p(ev + 4);
        if (type != 4 && type != 8 && type != 0x100)
            Log("wlan: <- scan event %lx reason %lu", (unsigned long)type, (unsigned long)reason);
        if (type == 2) {
            g_wl_scan_done = 1;
            Log("wlan: SCAN COMPLETE: %lu networks, %lu management frames", (unsigned long)g_wl_nbss, (unsigned long)g_wl_mgmt);
            {
                ULONG h;
                for (h = 0; h < 24 && g_wl_hid[h]; h++)
                    Log("wlan: event %06lx x %lu", (unsigned long)g_wl_hid[h], (unsigned long)g_wl_hn[h]);
                for (h = 0; h < 32; h++)
                    if (g_htt_hist[h])
                        Log("wlan: HTT type %lu x %lu", (unsigned long)h, (unsigned long)g_htt_hist[h]);
                Log("wlan: HTT rx alloc idx %lu, state %lu", (unsigned long)(g_htt_idx ? *g_htt_idx : 0), (unsigned long)g_htt_state);
                NetBegin();
            }
        }
    } else if (id == 0x7001) {
        WmiMgmt(p, n);
    } else if (id == 0x5001 && (ev = WmiTlvFind(p, n, 40, &l)) != NULL && l >= 16) {
        NetVdevStarted(Le32p(ev + 12));
    } else if (g_wl_rxlog++ < 16) {
        Log("wlan: WMI event %06lx, %lu B", (unsigned long)id, (unsigned long)n);
    }
    WmiFlush();
}

static void HtcCtl(const UCHAR *p, ULONG n)
{
    USHORT id = n >= 2 ? (USHORT)(p[0] | (p[1] << 8)) : 0;

    if (id == 1 && n >= 8) {                            // READY: credits, credit size, max endpoints
        g_wl_credits = p[2] | (p[3] << 8);
        g_wl_credit_size = p[4] | (p[5] << 8);
        Log("wlan: <- HTC READY: credits %lu size %lu max endpoints %u (%lu B)", (unsigned long)g_wl_credits, (unsigned long)g_wl_credit_size, p[6], (unsigned long)n);
        g_wl_state = 1;
        HtcConnect(0x300, 8);                           // HTT data: no credit flow
    } else if (id == 3 && n >= 8) {                     // CONNECT response: service, status, eid, max message size
        USHORT svc = p[2] | (p[3] << 8), mx = p[6] | (p[7] << 8);
        Log("wlan: <- HTC CONNECT resp service %04x status %u eid %u max %u", svc, p[4], p[5], mx);
        if (p[4] != 0) {
            g_wl_state = -1;
            return;
        }
        if (svc == 0x300) {
            g_wl_htt_ep = p[5];
            g_wl_state = 2;
            HtcConnect(0x100, (USHORT)((g_wl_credits & 0xff) << 8));   // WMI control: all credits to it, credit flow on
        } else if (svc == 0x100) {
            UCHAR m[12] = { 5, 0 };                     // SETUP_COMPLETE_EX
            g_wl_wmi_ep = p[5];
            g_wl_ep_credits[p[5] & 7] = g_wl_credits;
            g_wl_state = 3;
            Log("wlan: -> HTC SETUP_COMPLETE_EX: %d", HtcSend(&g_ce0, 0, 0, m, 12));
        }
    } else {
        WlHex("htc ctl", p, n);
    }
}

static void WlFrame(const UCHAR *f, ULONG n)
{
    ULONG eid, len, tl = 0;

    if (n < 8)
        return;
    eid = f[0];
    len = f[2] | (f[3] << 8);
    if (len + 8 > n) {
        Log("wlan: short frame eid %lu len %lu got %lu", (unsigned long)eid, (unsigned long)len, (unsigned long)n);
        return;
    }
    if ((f[1] & 2) && f[4] && f[4] <= len) {            // trailer records: credit reports
        const UCHAR *t = f + 8 + len - f[4];
        ULONG o = 0;
        tl = f[4];
        while (o + 4 <= tl) {
            ULONG id = t[o], l = t[o + 1], k;
            if (id == 1)
                for (k = 0; k + 4 <= l && o + 4 + k + 4 <= tl; k += 4)
                    if (t[o + 4 + k] < 8)
                        g_wl_ep_credits[t[o + 4 + k]] += t[o + 4 + k + 1];
            o += 4 + l;
        }
    }
    if (len <= tl)
        return;
    if (eid == 0)
        HtcCtl(f + 8, len - tl);
    else if (eid == g_wl_wmi_ep)
        WlWmiRx(f + 8, len - tl);
    else if (eid == g_wl_htt_ep)
        HttRx(f + 8, len - tl);
    else if (g_wl_rx_n++ < 20)
        Log("wlan: frame eid %lu, %lu B", (unsigned long)eid, (unsigned long)(len - tl));
}

static void CePoll(CEPIPE *p)
{
    int guard;

    for (guard = 0; guard < 64; guard++) {
        volatile ULONG *d = (volatile ULONG *)(p->desc + 16 * p->sw);
        ULONG w = d[2], nb = w & 0xffff;

        if (!nb)
            return;
        WlDsb();
        g_wl_work++;
        WlFrame(p->buf + WL_BUF * p->sw, nb > WL_BUF ? WL_BUF : nb);
        d[2] = 0;
        p->sw = (p->sw + 1) & p->mask;
        p->wr = (p->wr + 1) & p->mask;                  // re-post the slot just consumed (same fixed buffer per index)
        WlDsb();
        Wr(WL_CE(p->ce), 0x40, p->wr);
    }
}

static void WlStart(void)
{
    PHYSICAL_ADDRESS lo, hi, bnd;

    if (g_wl_on)
        return;
    lo.QuadPart = 0; hi.QuadPart = 0xffffffffLL; bnd.QuadPart = 0;
    g_wl_pool = (UCHAR *)MmAllocateContiguousMemorySpecifyCache(WL_POOL_SIZE, lo, hi, bnd, MmNonCached);
    if (!g_wl_pool) { Log("wlan: no DMA pool"); return; }
    WlZero(g_wl_pool, WL_POOL_SIZE);
    g_wl_pool_pa = (ULONG)MmGetPhysicalAddress(g_wl_pool).QuadPart;
    g_wl_ce = Map(WL_MEM_PA, 0x250000);
    if (!g_wl_ce) { Log("wlan: CE map failed"); return; }
    if (WlSmmu())
        return;
    CeInitSrc(&g_ce0, 0, 16);
    CeInitSrc(&g_ce3, 3, 32);
    CeInitDst(&g_ce1, 1, 64);
    CeInitDst(&g_ce2, 2, 64);
    CeInitSrc(&g_ce4, 4, 64);                           // the rest as ath10k_snoc_init_pipes + rx_post: every target->host ring has buffers
    CeInitDst(&g_ce5, 5, 64);
    CeInitDst(&g_ce8, 8, 64);
    CeInitDst(&g_ce9, 9, 64);
    CeInitDst(&g_ce10, 10, 64);
    CeInitDst(&g_ce11, 11, 64);
    g_wl_on = 1;
    g_wl_t = NowUs();
    Log("wlan: copy engines ready, waiting for HTC READY");
}

static void WlService(void)
{
    static LONGLONG last;
    ULONG fsr;

    if (!g_wl_on)
        return;
    {
    LONGLONG poll0 = NowUs();
    g_st_loops++;
    CePoll(&g_ce1);
    CePoll(&g_ce2);
    CePoll(&g_ce5);
    CePoll(&g_ce8);
    CePoll(&g_ce9);
    CePoll(&g_ce10);
    CePoll(&g_ce11);
    g_st_poll_us += NowUs() - poll0;
    }
    if (g_wl_init_at && NowUs() >= g_wl_init_at) {      // INIT a moment after SERVICE_READY, as the ath10k work queue does
        g_wl_init_at = 0;
        WmiInit(NULL, g_wl_memcopy, g_wl_memlen, 0, g_wl_reorder);
    }
    if (g_htt_state == 1 && NowUs() - g_htt_t > 3000000) {
        Log("wlan: HTT VERSION_CONF timeout, scanning anyway");
        g_htt_state = 3;
        g_wl_scan_at = NowUs();
    }
    if (g_wl_scan_at && NowUs() >= g_wl_scan_at && (!g_nv_wanted || g_nv || NowUs() - g_wl_scan_at > 30000000)) {   // the vdev takes the adapter's MAC: wait for the adapter
        if (g_nv_wanted && !g_nv)
            Log("wlan: net: native adapter did not bind in 30 s, scanning without it");
        g_wl_scan_at = 0;
        WmiStartScan();
    }
    NetService();
    WmiFlush();
    fsr = Rd(g_wl_cb, 0x58);
    if (fsr & ~0x400u) {                                 // bit 10 reads back set permanently on this SMMU and is not a fault
        static ULONG nf;
        if (nf++ < 20)
        Log("wlan: smmu fault fsr %08lx far %08lx%08lx fsynr0 %08lx", (unsigned long)fsr, (unsigned long)Rd(g_wl_cb, 0x64), (unsigned long)Rd(g_wl_cb, 0x60),
            (unsigned long)Rd(g_wl_cb, 0x68));
        Wr(g_wl_cb, 0x58, fsr);
    }
    {
        static LONGLONG dump;
        static int nd;
        if (g_wl_state >= 3 && nd < 8 && NowUs() - dump > 5000000) {
            dump = NowUs();
            nd++;
            Log("wlan: dump %d: ce3 srri %lx swr %lx our wr %lu, ce0 srri %lx swr %lx, ce2 drri %lx dwr %lx sw %lu, credits %lu, q %lu/%lu", nd, (unsigned long)Rd(WL_CE(3), 0x44),
                (unsigned long)Rd(WL_CE(3), 0x3c), (unsigned long)g_ce3.wr, (unsigned long)Rd(WL_CE(0), 0x44), (unsigned long)Rd(WL_CE(0), 0x3c), (unsigned long)Rd(WL_CE(2), 0x48),
                (unsigned long)Rd(WL_CE(2), 0x40), (unsigned long)g_ce2.sw, (unsigned long)g_wl_ep_credits[g_wl_wmi_ep & 7], (unsigned long)g_wq_h, (unsigned long)g_wq_t);
        }
    }
    if (g_wl_state == 0 && NowUs() - last > 5000000) {
        last = NowUs();
        Log("wlan: still no HTC READY (%lu s): ce1 drri %lx dwr %lx, ce2 drri %lx dwr %lx, gfsr %08lx", (unsigned long)((NowUs() - g_wl_t) / 1000000),
            (unsigned long)Rd(WL_CE(1), 0x48), (unsigned long)Rd(WL_CE(1), 0x40), (unsigned long)Rd(WL_CE(2), 0x48), (unsigned long)Rd(WL_CE(2), 0x40),
            (unsigned long)Rd(g_wl_smmu, 0x48));
    }
}
