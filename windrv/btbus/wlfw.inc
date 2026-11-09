// J606F Wi-Fi step F: WLFW client (QMI service 69 of the modem's WLAN PD), the sequence of ath10k qmi.c + snoc.c, polled from the btbus thread.
//  NEW_SERVER 69 -> IND_REGISTER -> HOST_CAP -> MSA_INFO (wlan_msa_mem 0x51900000 + 1 MiB) -> 20 ms -> MSA_READY -> CAP ->
//  (MSA_READY_IND) BDF_DOWNLOAD in 6144-byte segments -> CAL_REPORT -> (FW_READY_IND) -> WLAN_CFG -> WLAN_MODE(mission).
//  DT has qcom,msa-fixed-perm: no hypervisor assign of the MSA region. Board data = C:\j606f\drv\bdwlan.bin. Nothing is written to the flash.
// Log lines start with "wlfw:".
#define WF_PORT         0x4001
#define WF_CLIENT_ID    0x4b4e454cu
#define WF_MSA_PA       0x51900000u
#define WF_MSA_SIZE     0x00100000u
#define WF_SEG          6144
#define WF_TIMEOUT_US   30000000

enum { WF_IDLE, WF_IND, WF_HCAP, WF_MSAINFO, WF_MSAWAIT, WF_MSARDY, WF_CAP, WF_WAITIND, WF_BDF, WF_CAL, WF_WAITFW, WF_CFG, WF_MODE, WF_UP, WF_FAIL };

static UCHAR g_wfb[WF_SEG + 128];
static ULONG g_wf_n, g_wf_node, g_wf_port, g_wf_expect;
static USHORT g_wf_txn = 1, g_wf_cur;
static int g_wf_state, g_wf_msa_ind, g_wf_fw_ind, g_wf_wlog;
static LONGLONG g_wf_t;
static UCHAR *g_bdf;
static ULONG g_bdf_len, g_bdf_off, g_bdf_seg, g_bdf_end;

static void WfHdr(USHORT id)
{
    g_wf_cur = g_wf_txn++;
    g_wfb[0] = 0;                                       // request
    g_wfb[1] = (UCHAR)g_wf_cur; g_wfb[2] = (UCHAR)(g_wf_cur >> 8);
    g_wfb[3] = (UCHAR)id;       g_wfb[4] = (UCHAR)(id >> 8);
    g_wf_n = 7;
    g_wf_expect = id;
}

static void WfRaw(const void *v, ULONG len)
{
    ULONG i;

    for (i = 0; i < len && g_wf_n < sizeof(g_wfb); i++)
        g_wfb[g_wf_n++] = ((const UCHAR *)v)[i];
}

static void WfTlvH(UCHAR t, ULONG len)
{
    UCHAR h[3] = { t, (UCHAR)len, (UCHAR)(len >> 8) };

    WfRaw(h, 3);
}

static void WfT(UCHAR t, const void *v, ULONG len)
{
    WfTlvH(t, len);
    WfRaw(v, len);
}

static void WfSend(int state)
{
    ULONG l = g_wf_n - 7;

    g_wfb[5] = (UCHAR)l;
    g_wfb[6] = (UCHAR)(l >> 8);
    g_wf_state = state;
    g_wf_t = NowUs();
    if ((LONG)QrtrSendData(WF_PORT, g_wf_node, g_wf_port, g_wfb, g_wf_n) < 0) {
        Log("wlfw: send failed (msg %02lx, %lu B)", (unsigned long)g_wf_expect, (unsigned long)g_wf_n);
        g_wf_state = WF_FAIL;
    }
}

static void WfIndRegister(void)
{
    UCHAR one = 1;
    ULONG id = WF_CLIENT_ID;

    WfHdr(0x20);
    WfT(0x10, &one, 1);                                 // fw_ready_enable
    WfT(0x13, &one, 1);                                 // msa_ready_enable
    WfT(0x15, &id, 4);                                  // client_id
    WfSend(WF_IND);
    Log("wlfw: -> IND_REGISTER");
}

static void WfHostCap(void)
{
    ULONG zero = 0;

    WfHdr(0x34);
    WfT(0x10, &zero, 4);                                // daemon_support = 0
    WfSend(WF_HCAP);
    Log("wlfw: -> HOST_CAP");
}

static void WfMsaInfo(void)
{
    U64 a = WF_MSA_PA;
    ULONG s = WF_MSA_SIZE;

    WfHdr(0x2d);
    WfT(0x01, &a, 8);
    WfT(0x02, &s, 4);
    WfSend(WF_MSAINFO);
    Log("wlfw: -> MSA_INFO %08lx + %lx", (unsigned long)WF_MSA_PA, (unsigned long)WF_MSA_SIZE);
}

static void WfBdfNext(void)
{
    ULONG left = g_bdf_len - g_bdf_off, n = left > WF_SEG ? WF_SEG : left, zero = 0, tot = g_bdf_len;
    UCHAR one = 1, h[2];

    g_bdf_end = left <= WF_SEG;
    WfHdr(0x25);
    WfT(0x01, &one, 1);                                 // valid
    WfT(0x10, &zero, 4);                                // file_id
    WfT(0x11, &tot, 4);                                 // total_size
    WfT(0x12, &g_bdf_seg, 4);                           // seg_id
    WfTlvH(0x13, 2 + n);                                // data: u16 len + bytes
    h[0] = (UCHAR)n; h[1] = (UCHAR)(n >> 8);
    WfRaw(h, 2);
    WfRaw(g_bdf + g_bdf_off, n);
    one = g_bdf_end ? 1 : 0;
    WfT(0x14, &one, 1);                                 // end
    g_bdf_off += n;
    WfSend(WF_BDF);
}

static void WfBdfStart(void)
{
    if (!g_bdf) {
        Log("wlfw: no board data (C:\\j606f\\drv\\bdwlan.bin)");
        g_wf_state = WF_FAIL;
        return;
    }
    g_bdf_off = g_bdf_seg = 0;
    Log("wlfw: -> BDF_DOWNLOAD %lu B", (unsigned long)g_bdf_len);
    WfBdfNext();
}

static void WfCalReport(void)
{
    UCHAR zero = 0;

    WfHdr(0x26);
    WfT(0x01, &zero, 1);                                // meta_data_len 0
    WfSend(WF_CAL);
    Log("wlfw: -> CAL_REPORT");
}

static void WfArr(UCHAR t, ULONG count, const void *v, ULONG bytes)
{
    UCHAR c = (UCHAR)count;

    WfTlvH(t, 1 + bytes);
    WfRaw(&c, 1);
    WfRaw(v, bytes);
}

static void WfWlanCfg(void)
{
    static const ULONG tgt[12][5] = {                   // pipe, dir (1 in, 2 out, 3 inout, 4 h2h), nentries, nbytes_max, flags (snoc.c)
        { 0, 2, 32, 2048, 0 }, { 1, 1, 32, 2048, 0 }, { 2, 1, 64, 2048, 0 }, { 3, 2, 32, 2048, 0 },
        { 4, 2, 256, 256, 8 }, { 5, 2, 1024, 64, 8 }, { 6, 3, 32, 16384, 0 }, { 7, 4, 0, 0, 8 },
        { 8, 1, 32, 2048, 0 }, { 9, 1, 32, 2048, 0 }, { 10, 1, 32, 2048, 0 }, { 11, 1, 32, 2048, 0 },
    };
    static const ULONG svc[21][3] = {                   // service, dir, pipe (target_service_to_ce_map_wlan, with its terminator)
        { 0x104, 2, 3 }, { 0x104, 1, 2 }, { 0x102, 2, 3 }, { 0x102, 1, 2 }, { 0x101, 2, 3 }, { 0x101, 1, 2 }, { 0x103, 2, 3 }, { 0x103, 1, 2 },
        { 0x100, 2, 3 }, { 0x100, 1, 2 }, { 0x001, 2, 0 }, { 0x001, 1, 2 }, { 0xfe00, 2, 0 }, { 0xfe00, 1, 2 }, { 0x300, 2, 4 }, { 0x300, 1, 1 },
        { 0xfe00, 2, 5 }, { 0x301, 1, 9 }, { 0x302, 1, 10 }, { 0x600, 1, 11 }, { 0, 0, 0 },
    };
    static const USHORT shadow[12][2] = {               // ce_id, reg offset (SRC_WR_IDX 0x3c, DST_WR_IDX 0x40)
        { 0, 0x3c }, { 3, 0x3c }, { 4, 0x3c }, { 5, 0x3c }, { 7, 0x3c }, { 1, 0x40 }, { 2, 0x40 }, { 7, 0x40 }, { 8, 0x40 }, { 9, 0x40 }, { 10, 0x40 }, { 11, 0x40 },
    };

    WfHdr(0x23);
    WfArr(0x11, 12, tgt, sizeof(tgt));
    WfArr(0x12, 21, svc, sizeof(svc));
    WfArr(0x13, 12, shadow, sizeof(shadow));
    WfSend(WF_CFG);
    Log("wlfw: -> WLAN_CFG (%lu B)", (unsigned long)g_wf_n);
}

static void WfWlanMode(void)
{
    ULONG mode = 0;                                     // QMI_WLFW_MISSION_V01
    UCHAR dbg = 0;

    WfHdr(0x22);
    WfT(0x01, &mode, 4);
    WfT(0x10, &dbg, 1);                                 // hw_debug
    WfSend(WF_MODE);
    Log("wlfw: -> WLAN_MODE mission");
}

static void WfText(char *d, ULONG dmax, const UCHAR *s, ULONG n)
{
    ULONG i;

    for (i = 0; i < n && i + 1 < dmax && s[i]; i++)
        d[i] = (s[i] >= 32 && s[i] < 127) ? (char)s[i] : '?';
    d[i] = 0;
}

static void WfCapParse(const UCHAR *pl, ULONG plen)
{
    const UCHAR *v;
    ULONG l;
    char ts[40], bid[140];

    v = QmFind(pl, plen, 0x10, &l);
    if (v && l >= 8)
        Log("wlfw: chip_id %lx family %lx", (unsigned long)Le32p(v), (unsigned long)Le32p(v + 4));
    v = QmFind(pl, plen, 0x11, &l);
    if (v && l >= 4)
        Log("wlfw: board_id %lx", (unsigned long)Le32p(v));
    v = QmFind(pl, plen, 0x12, &l);
    if (v && l >= 4)
        Log("wlfw: soc_id %lx", (unsigned long)Le32p(v));
    v = QmFind(pl, plen, 0x13, &l);
    if (v && l >= 5) {
        WfText(ts, sizeof(ts), v + 5, l - 5 < v[4] ? l - 5 : v[4]);
        Log("wlfw: fw_version %lx timestamp '%s'", (unsigned long)Le32p(v), ts);
    }
    v = QmFind(pl, plen, 0x14, &l);
    if (v) {
        WfText(bid, sizeof(bid), v, l);
        Log("wlfw: fw_build_id '%s'", bid);
    }
    v = QmFind(pl, plen, 0x15, &l);
    if (v && l >= 1)
        Log("wlfw: num_macs %u", v[0]);
}

// read-only look at the SMMU (stream 0x1a0 = WLAN CE) and the copy engines, after WLAN_MODE as ath10k reads them
static void WfProbe(void)
{
    volatile UCHAR *smmu = Map(0x0c600000, 0x2000), *ce = Map(0x0c800000 + 0x240000, 0x1000 * 12);
    ULONG i, n, smr, s2, id, mask, hits = 0, vld = 0;

    if (smmu) {
        n = Rd(smmu, 0x20) & 0xff;                      // sIDR0.NUMSMRG
        Log("wlfw: smmu scr0 %08lx idr0 %08lx idr1 %08lx idr2 %08lx smrg %lu", (unsigned long)Rd(smmu, 0), (unsigned long)Rd(smmu, 0x20),
            (unsigned long)Rd(smmu, 0x24), (unsigned long)Rd(smmu, 0x28), (unsigned long)n);
        for (i = 0; i < n && i < 128; i++) {
            smr = Rd(smmu, 0x800 + 4 * i);
            if (!(smr & 0x80000000))
                continue;
            vld++;
            id = smr & 0x7fff;
            mask = (smr >> 16) & 0x7fff;
            s2 = Rd(smmu, 0xc00 + 4 * i);
            if (vld <= 24 || ((id ^ 0x1a0) & ~mask) == 0)
                Log("wlfw: smr[%lu] id %lx mask %lx s2cr %08lx%s", (unsigned long)i, (unsigned long)id, (unsigned long)mask, (unsigned long)s2,
                    ((id ^ 0x1a0) & ~mask) == 0 ? " <- WLAN" : "");
            if (((id ^ 0x1a0) & ~mask) == 0)
                hits++;
        }
        Log("wlfw: smmu valid smrs %lu, WLAN matches %lu", (unsigned long)vld, (unsigned long)hits);
        MmUnmapIoSpace((PVOID)smmu, 0x2000);
    }
    if (ce) {
        for (i = 0; i < 12; i++) {
            volatile UCHAR *c = ce + 0x1000 * i;
            Log("wlfw: ce%lu sr %08lx sz %lx dr %08lx sz %lx ctrl %08lx ie %08lx swr %lx dwr %lx srri %lx drri %lx", (unsigned long)i,
                (unsigned long)Rd(c, 0), (unsigned long)Rd(c, 8), (unsigned long)Rd(c, 0xc), (unsigned long)Rd(c, 0x14), (unsigned long)Rd(c, 0x18),
                (unsigned long)Rd(c, 0x2c), (unsigned long)Rd(c, 0x3c), (unsigned long)Rd(c, 0x40), (unsigned long)Rd(c, 0x44), (unsigned long)Rd(c, 0x48));
        }
        MmUnmapIoSpace((PVOID)ce, 0x1000 * 12);
    }
}

static void WfRx(ULONG node, ULONG port, const UCHAR *pl, ULONG plen)
{
    UCHAR type;
    USHORT txn, id;
    const UCHAR *r, *v;
    ULONG l, result, error, i;

    (void)node; (void)port;
    if (plen < 7)
        return;
    type = pl[0];
    txn = pl[1] | (pl[2] << 8);
    id = pl[3] | (pl[4] << 8);
    if (type == 4) {
        if (id == 0x2b) {
            Log("wlfw: <- MSA_READY_IND");
            g_wf_msa_ind = 1;
            if (g_wf_state == WF_WAITIND)
                WfBdfStart();
        } else if (id == 0x21) {
            Log("wlfw: <- FW_READY_IND");
            g_wf_fw_ind = 1;
            if (g_wf_state == WF_WAITFW)
                WfWlanCfg();
        } else {
            Log("wlfw: <- indication %02x (%lu B)", id, (unsigned long)plen);
        }
        return;
    }
    if (type != 2 || id != g_wf_expect || txn != g_wf_cur) {
        Log("wlfw: unexpected type %u msg %02x txn %u (want %02lx txn %u, state %d)", type, id, txn, (unsigned long)g_wf_expect, g_wf_cur, g_wf_state);
        return;
    }
    r = QmFind(pl, plen, 2, &l);
    result = (r && l >= 4) ? (ULONG)(r[0] | (r[1] << 8)) : 0xffff;
    error = (r && l >= 4) ? (ULONG)(r[2] | (r[3] << 8)) : 0xffff;
    if (g_wf_state != WF_BDF || g_bdf_seg == 0 || g_bdf_end || result)
        Log("wlfw: <- msg %02x result %lu error %lu (state %d)", id, (unsigned long)result, (unsigned long)error, g_wf_state);
    if (result && g_wf_state != WF_HCAP && !(g_wf_state == WF_BDF && g_bdf_end && result == 1)) {
        g_wf_state = WF_FAIL;
        return;
    }
    switch (g_wf_state) {
    case WF_IND:
        v = QmFind(pl, plen, 0x10, &l);
        if (v && l >= 8)
            Log("wlfw: fw_status %08lx%08lx", (unsigned long)Le32p(v + 4), (unsigned long)Le32p(v));
        WfHostCap();
        break;
    case WF_HCAP:
        WfMsaInfo();
        break;
    case WF_MSAINFO:
        v = QmFind(pl, plen, 3, &l);
        if (v && l >= 1)
            for (i = 0; i < v[0] && 1 + 13 * (i + 1) <= l; i++)
                Log("wlfw: msa region %08lx%08lx size %lx secure %u", (unsigned long)Le32p(v + 5 + 13 * i), (unsigned long)Le32p(v + 1 + 13 * i),
                    (unsigned long)Le32p(v + 9 + 13 * i), v[13 + 13 * i]);
        g_wf_state = WF_MSAWAIT;
        g_wf_t = NowUs();
        break;
    case WF_MSARDY:
        WfHdr(0x24);
        WfSend(WF_CAP);
        Log("wlfw: -> CAP");
        break;
    case WF_CAP:
        WfCapParse(pl, plen);
        if (g_wf_msa_ind) {
            WfBdfStart();
        } else {
            g_wf_state = WF_WAITIND;
            g_wf_t = NowUs();
        }
        break;
    case WF_BDF:
        if (g_bdf_end) {
            Log("wlfw: BDF done, %lu segments", (unsigned long)(g_bdf_seg + 1));
            WfCalReport();
        } else {
            g_bdf_seg++;
            WfBdfNext();
        }
        break;
    case WF_CAL:
        g_wf_state = WF_WAITFW;
        g_wf_t = NowUs();
        if (g_wf_fw_ind)
            WfWlanCfg();
        break;
    case WF_CFG:
        WfWlanMode();
        break;
    case WF_MODE:
        g_wf_state = WF_UP;
        Log("wlfw: WLAN firmware in mission mode");
        WfProbe();
        WlStart();
        break;
    default:
        break;
    }
}

static void WfArrive(ULONG node, ULONG port)
{
    if (g_wf_state != WF_IDLE && g_wf_state != WF_FAIL)
        return;
    g_wf_node = node;
    g_wf_port = port;
    g_wf_msa_ind = g_wf_fw_ind = g_wf_wlog = 0;
    WfIndRegister();
}

static void WfGone(void)
{
    Log("wlfw: service gone (state %d)", g_wf_state);
    g_wf_state = WF_IDLE;
}

static void WfService(void)
{
    LONGLONG dt;

    WlService();
    if (g_wf_state == WF_IDLE || g_wf_state == WF_FAIL || g_wf_state == WF_UP)
        return;
    dt = NowUs() - g_wf_t;
    if (g_wf_state == WF_MSAWAIT) {
        if (dt >= 20000) {
            WfHdr(0x2e);
            WfSend(WF_MSARDY);
            Log("wlfw: -> MSA_READY");
        }
    } else if (g_wf_state == WF_WAITIND) {
        if (dt > 3000000) {
            Log("wlfw: no MSA_READY_IND in 3 s, sending board data anyway");
            WfBdfStart();
        }
    } else if (dt > WF_TIMEOUT_US) {
        Log("wlfw: timeout in state %d (want msg %02lx)", g_wf_state, (unsigned long)g_wf_expect);
        g_wf_state = WF_FAIL;
    }
}

static void WfInit(void)
{
    g_bdf = LoadWhole(L"\\??\\C:\\j606f\\drv\\bdwlan.bin", &g_bdf_len, 256u << 10);
    NetInit();
    Log("wlfw: board data %lu B, l8 (cx-mx 0.64 V) %s", (unsigned long)g_bdf_len, Ldo(8, 1, 640000) == 0 ? "on" : "FAILED");
}
