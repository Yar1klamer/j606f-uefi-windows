// J606F Wi-Fi step B: GLINK over SMEM to the modem (MPSS) + first QRTR contact, polled from the btbus hardware thread.
// As Linux qcom_glink_smem.c + qcom_glink_native.c (features INTENT_REUSE, with intents) and net/qrtr (af_qrtr.c, ns.c):
//  - APPS/MPSS SMEM partition (hosts 0/1): item 478 = descriptor (32 B: tx tail, tx head, rx tail, rx head), item 479 = our TX FIFO (16 KiB,
//    allocated by us like qcom_smem_alloc_private), item 480 = RX FIFO (allocated by the modem). Kick = APCS_GLB + 8, bit 12 (DT mboxes apcs 12).
//  - VERSION(1, INTENT_REUSE) -> VERSION_ACK; remote OPEN -> OPEN_ACK; we OPEN "IPCRTR" (lcid 1) and queue 8 RX intents of 4 KiB.
//  - QRTR v1 (32-byte header): the modem's HELLO is answered with HELLO (as qrtr ns say_hello); NEW_SERVER / NEW_LOOKUP / DATA are logged.
// Only SMEM (shared RAM) is written. Started after a successful MPSS boot (flag mpss.arm). Log lines start with "glink:" / "qrtr:".
#define GM_DESC_ITEM    478
#define GM_TXF_ITEM     479
#define GM_RXF_ITEM     480
#define GM_TXF_SIZE     16384
#define GM_KICK_BIT     12
#define GM_INTENT_SZ    4096
#define GM_NINTENT      8

static volatile UCHAR *g_gm_desc, *g_gm_txf, *g_gm_rxf;
static ULONG g_gm_rxlen;
static int g_gm_on, g_gm_ver, g_gm_open_ack, g_gm_ipcrtr_rcid = -1, g_gm_hello_sent;
static USHORT g_gm_lcid = 1;
static LONGLONG g_gm_t0, g_gm_last_ver;
static UCHAR g_gm_rx[16384 + 64];
static ULONG g_gm_rx_n, g_gm_rx_iid;
#define GM_RI 64
static ULONG g_gm_rintent_iid[GM_RI], g_gm_rintent_sz[GM_RI], g_gm_rintent_busy[GM_RI], g_gm_rintents;   // remote intents for our TX

// generic qcom_smem_alloc_private / qcom_smem_get_private (uncached list only) in g_part
static volatile UCHAR *SmemItem(ULONG item, ULONG *size)
{
    ULONG p, end, w0, sz, padh, padd;

    if (!g_part)
        return NULL;
    end = ARd(g_part + 12);
    if (end > g_part_size)
        end = g_part_size;
    for (p = 32; p + 16 <= end; p += 16 + padh + sz) {
        w0 = ARd(g_part + p);
        sz = ARd(g_part + p + 4);
        padd = ARd(g_part + p + 8) & 0xffff;
        padh = ARd(g_part + p + 8) >> 16;
        if ((w0 & 0xffff) != 0xa5a5)
            return NULL;
        if ((w0 >> 16) == item) {
            if (size)
                *size = sz - padd;
            return g_part + p + 16 + padh;
        }
    }
    return NULL;
}

static volatile UCHAR *SmemAlloc(ULONG item, ULONG size)
{
    volatile UCHAR *m, *it;
    ULONG end, cached, owner = 0, i, asz = (size + 7) & ~7u;
    int err = 0;

    if ((it = SmemItem(item, NULL)) != NULL)
        return it;
    m = Map(TCSR_MUTEX3_PA, 0x1000);
    if (!m)
        return NULL;
    for (i = 0; i < 100000; i++) {
        AWr(m, 1);
        if ((owner = ARd(m)) == 1)
            break;
    }
    if (owner != 1) {
        MmUnmapIoSpace((PVOID)m, 0x1000);
        Log("glink: smem hwlock busy (%lu)", (unsigned long)owner);
        return NULL;
    }
    __asm__ volatile("msr daifset, #2" ::: "memory");
    it = NULL;
    end = ARd(g_part + 12);
    cached = ARd(g_part + 16);
    if (end > g_part_size || cached > g_part_size || end + 16 + asz > cached) {
        err = 1;
    } else {
        volatile UCHAR *e = g_part + end;
        // walk once more under the lock: the item may have appeared meanwhile
        ULONG p, w0, sz, padh;
        for (p = 32; p + 16 <= end; p += 16 + padh + sz) {
            w0 = ARd(g_part + p);
            sz = ARd(g_part + p + 4);
            padh = ARd(g_part + p + 8) >> 16;
            if ((w0 & 0xffff) != 0xa5a5) {
                err = 2;
                break;
            }
            if ((w0 >> 16) == item) {
                it = g_part + p + 16 + padh;
                break;
            }
        }
        if (!err && !it) {
            AWr(e, 0xa5a5u | (item << 16));
            AWr(e + 4, asz);
            AWr(e + 8, asz - size);
            AWr(e + 12, 0);
            __asm__ volatile("dsb sy" ::: "memory");
            AWr(g_part + 12, end + 16 + asz);
            it = e + 16;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    AWr(m, 0);
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    MmUnmapIoSpace((PVOID)m, 0x1000);
    if (err)
        Log("glink: smem alloc %lu (%lu) failed %d free %08lx cached %08lx", (unsigned long)item, (unsigned long)size, err,
            (unsigned long)end, (unsigned long)cached);
    else
        Log("glink: smem item %lu (%lu B) at %06lx", (unsigned long)item, (unsigned long)size, (unsigned long)(it - g_part));
    return err ? NULL : it;
}

static void GmKick(void)
{
    volatile UCHAR *apcs = Map(APCS_GLB_PA, 0x1000);

    if (!apcs)
        return;
    __asm__ volatile("dsb sy" ::: "memory");
    AWr(apcs + 8, 1u << GM_KICK_BIT);
    MmUnmapIoSpace((PVOID)apcs, 0x1000);
}

#define GM_TXT ARd(g_gm_desc + 0)
#define GM_TXH ARd(g_gm_desc + 4)
#define GM_RXT ARd(g_gm_desc + 8)
#define GM_RXH ARd(g_gm_desc + 12)

static ULONG GmRxAvail(void)
{
    ULONG h, t;

    if (!g_gm_rxf) {
        g_gm_rxf = SmemItem(GM_RXF_ITEM, &g_gm_rxlen);
        if (!g_gm_rxf)
            return 0;
        Log("glink: rx fifo (item 480) %lu B", (unsigned long)g_gm_rxlen);
    }
    h = GM_RXH;
    t = GM_RXT;
    if (h >= g_gm_rxlen || t >= g_gm_rxlen)
        return 0;
    return h < t ? g_gm_rxlen - t + h : h - t;
}

static void GmRxPeek(void *buf, ULONG off, ULONG count)
{
    ULONG t = GM_RXT + off, i;
    UCHAR *d = (UCHAR *)buf;

    for (i = 0; i < count; i++) {
        while (t >= g_gm_rxlen)
            t -= g_gm_rxlen;
        d[i] = g_gm_rxf[t++];
    }
}

static void GmRxAdvance(ULONG count)
{
    ULONG t = GM_RXT + count;

    while (t >= g_gm_rxlen)
        t -= g_gm_rxlen;
    AWr(g_gm_desc + 8, t);
}

static int GmTx(const void *hdr, ULONG hlen, const void *data, ULONG dlen)
{
    ULONG h = GM_TXH, t = GM_TXT, avail, i, tot = (hlen + dlen + 7) & ~7u;
    const UCHAR *p;

    avail = t <= h ? GM_TXF_SIZE - h + t : t - h;
    if (avail < tot + 16) {
        Log("glink: tx fifo full (avail %lu need %lu)", (unsigned long)avail, (unsigned long)tot);
        return -1;
    }
    for (p = (const UCHAR *)hdr, i = 0; i < hlen; i++) {
        g_gm_txf[h] = p[i];
        if (++h >= GM_TXF_SIZE)
            h = 0;
    }
    for (p = (const UCHAR *)data, i = 0; i < dlen; i++) {
        g_gm_txf[h] = p[i];
        if (++h >= GM_TXF_SIZE)
            h = 0;
    }
    h = (h + 7) & ~7u;
    if (h >= GM_TXF_SIZE)
        h -= GM_TXF_SIZE;
    __asm__ volatile("dsb sy" ::: "memory");
    AWr(g_gm_desc + 4, h);
    GmKick();
    return 0;
}

static int GmCmd(USHORT cmd, USHORT p1, ULONG p2)
{
    ULONG m[2];

    m[0] = cmd | ((ULONG)p1 << 16);
    m[1] = p2;
    return GmTx(m, 8, NULL, 0);
}

static void GmOpenIpcrtr(void)
{
    static const char name[8] = "IPCRTR";
    ULONG m[2], in[2 + 2 * GM_NINTENT], i;

    m[0] = 2 | ((ULONG)g_gm_lcid << 16);                 // OPEN lcid, name length incl. NUL
    m[1] = 7;
    GmTx(m, 8, name, 8);
    in[0] = 5 | ((ULONG)g_gm_lcid << 16);                // INTENT lcid, count
    in[1] = GM_NINTENT;
    for (i = 0; i < GM_NINTENT; i++) {
        in[2 + 2 * i] = GM_INTENT_SZ;
        in[3 + 2 * i] = i + 1;
    }
    GmTx(in, 8 + 8 * GM_NINTENT, NULL, 0);
    Log("glink: OPEN IPCRTR lcid %u + %u intents of %u", g_gm_lcid, GM_NINTENT, GM_INTENT_SZ);
}

// QRTR packets out on IPCRTR: one TX_DATA chunk each, into a free remote intent of sufficient size. Without one the packet waits in a small
// queue and an intent of that size is requested (RX_INTENT_REQ); GmService flushes the queue when the modem queues new intents.
#define GM_QN 24
static UCHAR g_gm_q[GM_QN][32 + 6300];
static ULONG g_gm_qlen[GM_QN], g_gm_qh, g_gm_qt, g_gm_req_pending;
static ULONG g_gm_drop;

static int GmSendPkt(const UCHAR *pkt, ULONG len)
{
    ULONG m[4], i, best = 99;

    for (i = 0; i < g_gm_rintents; i++)
        if (!g_gm_rintent_busy[i] && g_gm_rintent_sz[i] >= len && (best == 99 || g_gm_rintent_sz[i] < g_gm_rintent_sz[best]))
            best = i;
    if (best == 99)
        return -1;
    m[0] = 9 | ((ULONG)g_gm_lcid << 16);                // TX_DATA lcid, iid; chunk, left 0
    m[1] = g_gm_rintent_iid[best];
    m[2] = len;
    m[3] = 0;
    if (GmTx(m, 16, pkt, len))
        return -2;
    g_gm_rintent_busy[best] = 1;                        // until RX_DONE(_W_REUSE)
    return 0;
}

static void GmFlush(void)
{
    while (g_gm_qh != g_gm_qt) {
        ULONG k = g_gm_qt % GM_QN;
        if (GmSendPkt(g_gm_q[k], g_gm_qlen[k]))
            break;
        g_gm_qt++;
        g_gm_req_pending = 0;
    }
    if (g_gm_qh != g_gm_qt && !g_gm_req_pending) {
        g_gm_req_pending = g_gm_qlen[g_gm_qt % GM_QN];
        GmCmd(7, g_gm_lcid, g_gm_req_pending);          // RX_INTENT_REQ size
    }
}

static int QrtrTx(ULONG type, ULONG src_port, ULONG dst_node, ULONG dst_port, const void *pl, ULONG plen)
{
    ULONG h[8], i, k;
    UCHAR *pkt;

    if (g_gm_ipcrtr_rcid < 0 || 32 + plen > sizeof(g_gm_q[0]))
        return -1;
    if (g_gm_qh - g_gm_qt >= GM_QN) {
        g_gm_drop++;
        Log("qrtr: tx queue full, packet dropped (%lu)", (unsigned long)g_gm_drop);
        return -1;
    }
    h[0] = 1;
    h[1] = type;
    h[2] = 1;                                           // our node id (APPS)
    h[3] = src_port;
    h[4] = 0;
    h[5] = plen;
    h[6] = dst_node;
    h[7] = dst_port;
    k = g_gm_qh % GM_QN;
    pkt = g_gm_q[k];
    for (i = 0; i < 32; i++)
        pkt[i] = ((UCHAR *)h)[i];
    for (i = 0; i < plen; i++)
        pkt[32 + i] = ((const UCHAR *)pl)[i];
    g_gm_qlen[k] = 32 + plen;
    g_gm_qh++;
    GmFlush();
    return 0;
}

static ULONG QrtrSendData(ULONG src_port, ULONG dst_node, ULONG dst_port, const void *pl, ULONG plen)
{
    return (ULONG)QrtrTx(1, src_port, dst_node, dst_port, pl, plen);
}

#include "qmisvc.inc"

static void QrtrAnnounce(ULONG node)
{
    static const ULONG sv[3][3] = { { 14, 1, 1 }, { 64, 0x101, 2 }, { 4096, 1, 3 } };   // service, ver|inst<<8, port
    ULONG c[5], i;

    for (i = 0; i < 3; i++) {
        c[0] = 4;
        c[1] = sv[i][0];
        c[2] = sv[i][1];
        c[3] = 1;
        c[4] = sv[i][2];
        QrtrTx(4, 0xfffffffe, node, 0xfffffffe, c, 20);
    }
    Log("qrtr: announced rmtfs (14) port 1, servreg locator (64) port 2, tftp (4096) port 3 to node %lu", (unsigned long)node);
}

static void QrtrRx(const UCHAR *d, ULONG n)
{
    const ULONG *h = (const ULONG *)d;
    ULONG type, plen;

    if (n < 32 || h[0] != 1) {
        Log("qrtr: bad packet %lu B ver %lu", (unsigned long)n, n >= 4 ? (unsigned long)h[0] : 0);
        return;
    }
    type = h[1];
    plen = h[5];
    if (type == 2) {                                    // HELLO
        Log("qrtr: HELLO from node %lu port %lx", (unsigned long)h[2], (unsigned long)h[3]);
        if (!g_gm_hello_sent) {
            ULONG c[5] = { 2, 0, 0, 0, 0 };
            g_gm_hello_sent = 1;
            Log("qrtr: send HELLO -> %d", QrtrTx(2, 0xfffffffe, h[2], 0xfffffffe, c, 20));
            QrtrAnnounce(h[2]);
        }
    } else if (type == 1 && h[6] == 1) {                // DATA for us
        if (plen > n - 32)
            plen = n - 32;
        SvcRx(h[2], h[3], h[7], d + 32, plen);
        if (h[4]) {                                     // confirm_rx -> RESUME_TX {cmd 7, client {our node, port}}
            ULONG c[5] = { 7, 1, h[7], 0, 0 };
            QrtrTx(7, h[7], h[2], h[3], c, 20);
        }
    } else if (type == 6 || type == 7) {                // DEL_CLIENT / RESUME_TX: nothing to do
    } else if ((type == 4 || type == 5 || type == 10) && plen >= 20) {
        const ULONG *c = (const ULONG *)(d + 32);       // ctrl: cmd, service, instance, node, port
        Log("qrtr: %s service %lu ver %lu inst %lu node %lu port %lu", type == 4 ? "NEW_SERVER" : type == 5 ? "DEL_SERVER" : "NEW_LOOKUP",
            (unsigned long)c[1], (unsigned long)(c[2] & 0xff), (unsigned long)(c[2] >> 8), (unsigned long)c[3], (unsigned long)c[4]);
        if (type == 4 && c[1] == 69)
            WfArrive(c[3], c[4]);
        else if (type == 5 && c[1] == 69)
            WfGone();
    } else {
        ULONG i, k = 0;
        char line[120];
        static const char hx[] = "0123456789abcdef";
        for (i = 32; i < n && i < 32 + 32 && k + 4 < sizeof(line); i++) {
            line[k++] = ' ';
            line[k++] = hx[d[i] >> 4];
            line[k++] = hx[d[i] & 15];
        }
        line[k] = 0;
        Log("qrtr: type %lu %lu:%lx -> %lu:%lx len %lu:%s", (unsigned long)type, (unsigned long)h[2], (unsigned long)h[3], (unsigned long)h[6],
            (unsigned long)h[7], (unsigned long)plen, line);
    }
}

static void GmPoll(void)
{
    int guard;

    for (guard = 0; guard < 64; guard++) {
        ULONG avail = GmRxAvail(), m[4];
        USHORT cmd, p1;
        ULONG p2;

        if (avail < 8)
            return;
        GmRxPeek(m, 0, 8);
        cmd = (USHORT)(m[0] & 0xffff);
        p1 = (USHORT)(m[0] >> 16);
        p2 = m[1];
        switch (cmd) {
        case 0:                                         // VERSION
            Log("glink: rx VERSION %u features %lx", p1, (unsigned long)p2);
            GmRxAdvance(8);
            GmCmd(1, 1, p2 & 1);
            if (!g_gm_ver) {
                g_gm_ver = 1;
                GmOpenIpcrtr();
            }
            break;
        case 1:                                         // VERSION_ACK
            Log("glink: rx VERSION_ACK %u features %lx", p1, (unsigned long)p2);
            GmRxAdvance(8);
            if (!g_gm_ver) {
                g_gm_ver = 1;
                GmOpenIpcrtr();
            }
            break;
        case 2: {                                       // OPEN rcid, name_len
            ULONG tot = (8 + p2 + 7) & ~7u, i;
            char name[33];
            if (avail < tot)
                return;
            GmRxPeek(name, 8, p2 < 32 ? p2 : 32);
            name[p2 < 32 ? p2 : 32] = 0;
            for (i = 0; name[i]; i++)
                if (name[i] < 32 || name[i] > 126)
                    name[i] = '?';
            Log("glink: rx OPEN rcid %u '%s'", p1, name);
            GmRxAdvance(tot);
            GmCmd(4, p1, 0);                            // OPEN_ACK
            if (name[0] == 'I' && name[1] == 'P' && name[2] == 'C' && name[3] == 'R' && name[4] == 'T' && name[5] == 'R' && !name[6])
                g_gm_ipcrtr_rcid = p1;
            break;
        }
        case 4:                                         // OPEN_ACK lcid
            Log("glink: rx OPEN_ACK lcid %u", p1);
            GmRxAdvance(8);
            if (p1 == g_gm_lcid)
                g_gm_open_ack = 1;
            break;
        case 3:                                         // CLOSE
            Log("glink: rx CLOSE rcid %u", p1);
            GmRxAdvance(8);
            GmCmd(11, p1, 0);
            if ((int)p1 == g_gm_ipcrtr_rcid)
                g_gm_ipcrtr_rcid = -1;
            break;
        case 5: {                                       // INTENT rcid, count; {size, iid} x count
            ULONG tot = 8 + 8 * p2, i, e[2];
            if (avail < tot)
                return;
            for (i = 0; i < p2; i++) {
                GmRxPeek(e, 8 + 8 * i, 8);
                {
                    ULONG k = g_gm_rintents;
                    if (k >= GM_RI) {                   // table full: replace the smallest free intent (08.11: a granted 4 KiB intent was lost)
                        ULONG j;
                        k = GM_RI;
                        for (j = 0; j < GM_RI; j++)
                            if (!g_gm_rintent_busy[j] && (k == GM_RI || g_gm_rintent_sz[j] < g_gm_rintent_sz[k]))
                                k = j;
                        if (k == GM_RI || g_gm_rintent_sz[k] > e[0])
                            k = GM_RI;
                    } else {
                        g_gm_rintents++;
                    }
                    if (k < GM_RI) {
                        g_gm_rintent_sz[k] = e[0];
                        g_gm_rintent_busy[k] = 0;
                        g_gm_rintent_iid[k] = e[1];
                    }
                }
            }
            if (e[0] > 128 || g_gm_rintents < 20)
                Log("glink: rx INTENT rcid %u count %lu size %lu -> %lu", p1, (unsigned long)p2, (unsigned long)e[0], (unsigned long)g_gm_rintents);
            GmRxAdvance((tot + 7) & ~7u);
            break;
        }
        case 6:                                         // RX_DONE (intent consumed, not reused)
        case 14:                                        // RX_DONE_W_REUSE: intent is back
            GmRxAdvance(8);
            {
                ULONG i;
                for (i = 0; i < g_gm_rintents; i++)
                    if (g_gm_rintent_iid[i] == p2) {
                        if (cmd == 14) {
                            g_gm_rintent_busy[i] = 0;
                        } else {
                            g_gm_rintents--;
                            g_gm_rintent_iid[i] = g_gm_rintent_iid[g_gm_rintents];
                            g_gm_rintent_sz[i] = g_gm_rintent_sz[g_gm_rintents];
                            g_gm_rintent_busy[i] = g_gm_rintent_busy[g_gm_rintents];
                        }
                        break;
                    }
            }
            break;
        case 7:                                         // RX_INTENT_REQ: remote wants an intent of p2 bytes -> grant one
            Log("glink: rx RX_INTENT_REQ %lu", (unsigned long)p2);
            GmRxAdvance(8);
            {
                static ULONG next_iid = 100;
                ULONG in[4] = { 5 | ((ULONG)g_gm_lcid << 16), 1, p2 > GM_INTENT_SZ ? p2 : GM_INTENT_SZ, next_iid++ };
                GmTx(in, 16, NULL, 0);
                GmCmd(8, g_gm_lcid, 1);                 // RX_INTENT_REQ_ACK granted
            }
            break;
        case 8:                                         // RX_INTENT_REQ_ACK
            Log("glink: rx RX_INTENT_REQ_ACK %lu", (unsigned long)p2);
            GmRxAdvance(8);
            break;
        case 9:                                         // TX_DATA rcid, iid; chunk, left
        case 12: {                                      // TX_DATA_CONT
            ULONG chunk, left, tot, i;
            if (avail < 16)
                return;
            GmRxPeek(m, 0, 16);
            chunk = m[2];
            left = m[3];
            tot = (16 + chunk + 7) & ~7u;
            if (avail < tot)
                return;
            if (cmd == 9) {
                g_gm_rx_n = 0;
                g_gm_rx_iid = p2;
            }
            for (i = 0; i < chunk; i++) {
                UCHAR c;
                GmRxPeek(&c, 16 + i, 1);
                if (g_gm_rx_n < sizeof(g_gm_rx))
                    g_gm_rx[g_gm_rx_n++] = c;
            }
            GmRxAdvance(tot);
            if (!left) {
                GmCmd(14, g_gm_lcid, g_gm_rx_iid);      // RX_DONE_W_REUSE: our intent stays queued
                if ((int)p1 == g_gm_ipcrtr_rcid)
                    QrtrRx(g_gm_rx, g_gm_rx_n);
                else
                    Log("glink: data on rcid %u, %lu B", p1, (unsigned long)g_gm_rx_n);
            }
            break;
        }
        case 13:                                        // READ_NOTIF
        case 15:                                        // SIGNALS
            GmRxAdvance(cmd == 15 ? 8 : 8);
            break;
        default:
            Log("glink: rx unknown cmd %u p1 %u p2 %lx avail %lu -> drop", cmd, p1, (unsigned long)p2, (unsigned long)avail);
            GmRxAdvance(avail & ~7u);
            return;
        }
    }
}

static void GmStart(void)
{
    ULONG sz = 0;

    if (SmemOpenPart())
        return;
    SvcInit();
    g_gm_desc = SmemAlloc(GM_DESC_ITEM, 32);
    g_gm_txf = SmemAlloc(GM_TXF_ITEM, GM_TXF_SIZE);
    if (!g_gm_desc || !g_gm_txf || !SmemItem(GM_DESC_ITEM, &sz) || sz != 32) {
        Log("glink: no descriptor / tx fifo (desc size %lu)", (unsigned long)sz);
        return;
    }
    Log("glink: desc tx %lu/%lu rx %lu/%lu, rx fifo %s", (unsigned long)GM_TXT, (unsigned long)GM_TXH, (unsigned long)GM_RXT,
        (unsigned long)GM_RXH, SmemItem(GM_RXF_ITEM, NULL) ? "present" : "not yet");
    g_gm_on = 1;
    g_gm_t0 = g_gm_last_ver = NowUs();
    GmCmd(0, 1, 1);                                     // VERSION 1, features INTENT_REUSE
}

static void GmService(void)
{
    if (!g_gm_on)
        return;
    GmPoll();
    if (g_gm_qh != g_gm_qt)
        GmFlush();
    WfService();
    if (!g_gm_ver && NowUs() - g_gm_last_ver > 2000000 && NowUs() - g_gm_t0 < 30000000) {
        g_gm_last_ver = NowUs();
        GmCmd(0, 1, 1);
    }
}
