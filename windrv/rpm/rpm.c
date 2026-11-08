// J606F RPM client (Windows ARM64 kernel driver, test-signed, legacy service) — step 1 for Bluetooth / Wi-Fi power.
// The PM6125 LDOs that power the WCN3990 (BT: vddio L9, vddxo L16, vddrf L17, vddch0 L23 — stock bt_wcn3990 / pmOS DT) are owned by the RPM
// (run_36: L1-L16 have no apps SPMI channel). The RPM is reached over GLINK in the RPM message RAM (0x45f0000, 0x7000; TOC "grt0" at the end,
// FIFOs ap2r (tx) / r2ap (rx): [tail][head][fifo...]) with the APCS IPC kick (0xf111000 + 8, bit 0). Same protocol as Linux qcom_glink_rpm.c +
// qcom_glink_native.c (intentless) + smd-rpm.c + qcom_smd-regulator.c:
//   reset our tx head / rx tail -> VERSION(1, features 0) -> remote VERSION / VERSION_ACK -> remote OPEN "rpm_requests" (rcid) -> OPEN_ACK(rcid)
//   -> our OPEN (lcid 1, "rpm_requests") -> remote OPEN_ACK -> TX_DATA(lcid, iid 0, chunk, left 0) with the SMD-RPM request:
//   hdr {"req\0", len} + req {msg_id, flags 0 = active set, type "ldoa", id n, data_len} + KV {"swen" 4 1} {"uv" 4 uV};
//   the RPM answers with TX_DATA on its rcid: hdr {"req\0"} + {"msg#" 4 msg_id} [+ {"err\0" len text}].
// Polling only (no interrupt). Mode by service name: "...rpmr" = GLINK handshake only (no regulator request); "...rpmb" = handshake +
// BT LDOs on (L9 1.8 V, L16 1.704 V, L17 1.152 V, L23 3.2 V = Linux values in pmOS), SPMI read-back of L17..L23 status; on unload the LDOs
// are switched off again (swen 0). Nothing else is written. Log: C:\j606f\out\rpm.txt.
#include "j606f_wdm.h"

#ifndef RPM_LOG_PATH
#define RPM_LOG_PATH L"\\??\\C:\\j606f\\out\\rpm.txt"
#endif

static void LogRaw(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, RPM_LOG_PATH);
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &attrs, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(file, NULL, NULL, NULL, &iosb, (PVOID)text, len, NULL, NULL);
        ZwClose(file);
    }
}

static void Log(const char *fmt, ...)
{
    char line[300];
    __builtin_va_list ap;
    ULONG n = 0;

    __builtin_va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    __builtin_va_end(ap);
    line[sizeof(line) - 3] = 0;
    while (line[n])
        n++;
    line[n] = '\r';
    line[n + 1] = '\n';
    line[n + 2] = 0;
    LogRaw(line);
}

static ULONG Rd(volatile UCHAR *base, ULONG off)
{
    ULONG v = *(volatile ULONG *)(base + off);
    __asm__ volatile("dmb sy" ::: "memory");
    return v;
}

static void Wr(volatile UCHAR *base, ULONG off, ULONG v)
{
    __asm__ volatile("dmb sy" ::: "memory");
    *(volatile ULONG *)(base + off) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void SleepMs(LONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(0, FALSE, &t);
}

static volatile UCHAR *Map(ULONG pa, SIZE_T size)
{
    PHYSICAL_ADDRESS p;
    volatile UCHAR *va;

    p.QuadPart = pa;
    va = (volatile UCHAR *)MmMapIoSpaceEx(p, size, PAGE_READWRITE | PAGE_NOCACHE);
    if (!va)
        Log("map %08lx failed", (unsigned long)pa);
    return va;
}

// =====================================================================================================================================
// GLINK over RPM message RAM

#define MSGRAM_PA   0x045f0000
#define MSGRAM_SIZE 0x7000
#define APCS_PA     0x0f111000
#define TOC_MAGIC   0x67727430
#define ID_TX       0x61703272      // ap2r
#define ID_RX       0x72326170      // r2ap

#define CMD_VERSION     0
#define CMD_VERSION_ACK 1
#define CMD_OPEN        2
#define CMD_CLOSE       3
#define CMD_OPEN_ACK    4
#define CMD_TX_DATA     9
#define CMD_CLOSE_ACK   11
#define CMD_READ_NOTIF  13
#define CMD_SIGNALS     15

typedef struct {
    ULONG tail, head, fifo, len;    // offsets in msg RAM
} PIPE;

static volatile UCHAR *g_ram, *g_apcs;
static PIPE g_tx, g_rx;
static USHORT g_rcid, g_lcid = 1;
static int g_ver_ok, g_ropen, g_open_ack;
static volatile int g_close_ack;
static ULONG g_msg_id = 1, g_ack_id, g_ack_err;
static char g_err[40];

static void Kick(void)
{
    Wr(g_apcs, 8, 1);
}

static ULONG RxAvail(void)
{
    ULONG h = Rd(g_ram, g_rx.head), t = Rd(g_ram, g_rx.tail);
    if (h >= g_rx.len || t >= g_rx.len)
        return 0;
    return h < t ? g_rx.len - t + h : h - t;
}

static void RxPeek(void *buf, ULONG off, ULONG count)     // count multiple of 4
{
    ULONG t = Rd(g_ram, g_rx.tail) + off, i;
    ULONG *d = (ULONG *)buf;

    for (i = 0; i < count / 4; i++) {
        while (t >= g_rx.len)
            t -= g_rx.len;
        d[i] = Rd(g_ram, g_rx.fifo + t);
        t += 4;
    }
}

static void RxAdvance(ULONG count)
{
    ULONG t = Rd(g_ram, g_rx.tail) + count;
    while (t >= g_rx.len)
        t -= g_rx.len;
    Wr(g_ram, g_rx.tail, t);
}

static ULONG TxAvail(void)
{
    ULONG h = Rd(g_ram, g_tx.head), t = Rd(g_ram, g_tx.tail);
    return t <= h ? g_tx.len - h + t : t - h;
}

// writes hdr (8 or 16 bytes) + data, padded to 8; data length multiple of 4 is required by the callers
static int Tx(const ULONG *hdr, ULONG hlen, const ULONG *data, ULONG dlen)
{
    ULONG tot = hlen + dlen, pad = ((tot + 7) & ~7u) - tot, h, i, n;
    int w;

    for (w = 0; w < 100 && TxAvail() <= tot + pad + 8; w++)
        SleepMs(5);
    if (TxAvail() <= tot + pad + 8) {
        Log("tx: fifo full (avail %lu)", (unsigned long)TxAvail());
        return -1;
    }
    h = Rd(g_ram, g_tx.head);
    for (n = 0; n < 3; n++) {
        const ULONG *src = n == 0 ? hdr : n == 1 ? data : NULL;
        ULONG cnt = n == 0 ? hlen : n == 1 ? dlen : pad;
        for (i = 0; i < cnt / 4; i++) {
            Wr(g_ram, g_tx.fifo + h, src ? src[i] : 0);
            h += 4;
            if (h >= g_tx.len)
                h -= g_tx.len;
        }
    }
    Wr(g_ram, g_tx.head, h);
    Kick();
    return 0;
}

static int TxCmd(USHORT cmd, USHORT p1, ULONG p2)
{
    ULONG m[2];
    m[0] = cmd | ((ULONG)p1 << 16);
    m[1] = p2;
    return Tx(m, 8, NULL, 0);
}

static void HandleData(const ULONG *d, ULONG len)
{
    // smd-rpm response: hdr {service, length} then messages {type, length, payload}
    ULONG off = 8, end;

    if (len < 8)
        return;
    end = 8 + d[1];
    if (end > len)
        end = len;
    Log("rpm data: service %08lx length %lu", (unsigned long)d[0], (unsigned long)d[1]);
    while (off + 8 <= end) {
        ULONG type = d[off / 4], mlen = d[off / 4 + 1];
        if (type == 0x2367736d && mlen >= 4) {          // "msg#"
            g_ack_id = d[off / 4 + 2];
            Log("  ack msg_id %lu", (unsigned long)g_ack_id);
        } else if (type == 0x00727265) {               // "err\0"
            ULONG i;
            const char *s = (const char *)&d[off / 4 + 2];
            for (i = 0; i < mlen && i < sizeof(g_err) - 1; i++)
                g_err[i] = s[i];
            g_err[i] = 0;
            g_ack_err = 1;
            Log("  error: %s", g_err);
        } else {
            Log("  msg type %08lx len %lu", (unsigned long)type, (unsigned long)mlen);
        }
        off += 8 + ((mlen + 3) & ~3u);
    }
}

static ULONG g_buf[128];

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void RxPoll(void)
{
    int guard;

    for (guard = 0; guard < 32; guard++) {
        ULONG avail = RxAvail(), m[4];
        USHORT cmd, p1;
        ULONG p2;

        if (avail < 8)
            return;
        RxPeek(m, 0, 8);
        cmd = (USHORT)(m[0] & 0xffff);
        p1 = (USHORT)(m[0] >> 16);
        p2 = m[1];
        switch (cmd) {
        case CMD_VERSION:
            Log("rx VERSION %u features %lx", p1, (unsigned long)p2);
            RxAdvance(8);
            TxCmd(CMD_VERSION_ACK, 1, 0);
            g_ver_ok = 1;
            break;
        case CMD_VERSION_ACK:
            Log("rx VERSION_ACK %u features %lx", p1, (unsigned long)p2);
            RxAdvance(8);
            g_ver_ok = 1;
            break;
        case CMD_OPEN: {
            ULONG nl = p2, tot = (8 + nl + 7) & ~7u;
            char name[36];
            ULONG i;
            if (avail < tot)
                return;
            if (nl > 32)
                nl = 32;
            RxPeek(g_buf, 8, (nl + 3) & ~3u);
            for (i = 0; i < nl && i < 35; i++)
                name[i] = ((char *)g_buf)[i];
            name[i] = 0;
            Log("rx OPEN rcid %u name '%s'", p1, name);
            RxAdvance(tot);
            if (StrEq(name, "rpm_requests")) {
                g_rcid = p1;
                g_ropen = 1;
            }
            break;
        }
        case CMD_OPEN_ACK:
            Log("rx OPEN_ACK lcid %u", p1);
            RxAdvance(8);
            if (p1 == g_lcid)
                g_open_ack = 1;
            break;
        case CMD_TX_DATA: {
            ULONG chunk, left, tot;
            if (avail < 16)
                return;
            RxPeek(m, 0, 16);
            chunk = m[2];
            left = m[3];
            tot = (16 + chunk + 7) & ~7u;
            if (avail < tot)
                return;
            if (chunk <= sizeof(g_buf)) {
                RxPeek(g_buf, 16, (chunk + 3) & ~3u);
                Log("rx TX_DATA rcid %u chunk %lu left %lu", p1, (unsigned long)chunk, (unsigned long)left);
                HandleData(g_buf, chunk);
            } else {
                Log("rx TX_DATA too long %lu", (unsigned long)chunk);
            }
            RxAdvance(tot);
            break;
        }
        case CMD_CLOSE:
            Log("rx CLOSE rcid %u", p1);
            RxAdvance(8);
            TxCmd(CMD_CLOSE_ACK, p1, 0);
            break;
        case CMD_CLOSE_ACK:
            Log("rx CLOSE_ACK lcid %u", p1);
            RxAdvance(8);
            if (p1 == g_lcid)
                g_close_ack = 1;
            break;
        case CMD_READ_NOTIF:
        case CMD_SIGNALS:
            Log("rx cmd %u p1 %u p2 %lx", cmd, p1, (unsigned long)p2);
            RxAdvance(8);
            break;
        default:
            Log("rx unknown cmd %u p1 %u p2 %lx (avail %lu) - dropping all", cmd, p1, (unsigned long)p2, (unsigned long)avail);
            RxAdvance(avail & ~7u);
            return;
        }
    }
}

static int WaitFor(volatile int *flag, int ms)
{
    int i;
    for (i = 0; i < ms / 10; i++) {
        RxPoll();
        if (*flag)
            return 0;
        SleepMs(10);
    }
    return -1;
}

static int GlinkOpen(void)
{
    ULONG magic, count, i;

    g_ram = Map(MSGRAM_PA, MSGRAM_SIZE);
    g_apcs = Map(APCS_PA, 0x1000);
    if (!g_ram || !g_apcs)
        return -1;
    magic = Rd(g_ram, MSGRAM_SIZE - 256);
    count = Rd(g_ram, MSGRAM_SIZE - 256 + 4);
    if (magic != TOC_MAGIC || count > 20) {
        Log("toc: bad magic %08lx count %lu", (unsigned long)magic, (unsigned long)count);
        return -1;
    }
    for (i = 0; i < count; i++) {
        ULONG e = MSGRAM_SIZE - 256 + 8 + 12 * i, id = Rd(g_ram, e), off = Rd(g_ram, e + 4), sz = Rd(g_ram, e + 8);
        PIPE *p = id == ID_TX ? &g_tx : id == ID_RX ? &g_rx : NULL;
        if (!p)
            continue;
        if (off + 8 + sz > MSGRAM_SIZE || sz < 64 || (sz & 7)) {
            Log("toc: bad entry %08lx off %lx size %lx", (unsigned long)id, (unsigned long)off, (unsigned long)sz);
            return -1;
        }
        p->tail = off;
        p->head = off + 4;
        p->fifo = off + 8;
        p->len = sz;
    }
    if (!g_tx.len || !g_rx.len) {
        Log("toc: no ap2r/r2ap");
        return -1;
    }
    Log("pipes: tx off %lx len %lx (tail %lu head %lu), rx off %lx len %lx (tail %lu head %lu)", (unsigned long)g_tx.tail, (unsigned long)g_tx.len,
        (unsigned long)Rd(g_ram, g_tx.tail), (unsigned long)Rd(g_ram, g_tx.head), (unsigned long)g_rx.tail, (unsigned long)g_rx.len,
        (unsigned long)Rd(g_ram, g_rx.tail), (unsigned long)Rd(g_ram, g_rx.head));
    // Linux glink_rpm_probe writes 0 to our tx head and rx tail (fresh RPM). After an earlier session in this boot the RPM's tx tail is not 0
    // (run_39: 16), so: tx empty = head := tail (the RPM's read pointer); rx: drop old data = tail := head.
    Wr(g_ram, g_tx.head, Rd(g_ram, g_tx.tail));
    Wr(g_ram, g_rx.tail, Rd(g_ram, g_rx.head));
    TxCmd(CMD_VERSION, 1, 0);
    if (WaitFor(&g_ver_ok, 1000)) {
        Log("no VERSION answer (rx head %lu tail %lu, tx head %lu tail %lu)", (unsigned long)Rd(g_ram, g_rx.head), (unsigned long)Rd(g_ram, g_rx.tail),
            (unsigned long)Rd(g_ram, g_tx.head), (unsigned long)Rd(g_ram, g_tx.tail));
        return -1;
    }
    if (WaitFor(&g_ropen, 2000)) {
        Log("no remote OPEN rpm_requests");
        return -1;
    }
    TxCmd(CMD_OPEN_ACK, g_rcid, 0);
    {
        ULONG m[2 + 4];
        const char *nm = "rpm_requests";
        char *c = (char *)&m[2];
        int i2;
        m[0] = CMD_OPEN | ((ULONG)g_lcid << 16);
        m[1] = 13;                                      // strlen + 1
        for (i2 = 0; i2 < 16; i2++)
            c[i2] = i2 < 13 ? nm[i2] : 0;
        Tx(m, 8, &m[2], 16);                            // 8 + 13 -> padded 24
    }
    if (WaitFor(&g_open_ack, 1000)) {
        Log("no OPEN_ACK for our lcid");
        return -1;
    }
    Log("channel rpm_requests open: lcid %u rcid %u", g_lcid, g_rcid);
    return 0;
}

// one SMD-RPM request; waits for its ack
static int RpmWrite(ULONG type, ULONG id, const ULONG *kv, ULONG kvlen)
{
    ULONG pkt[32], hdr[4], i, n = 0, msg = g_msg_id++;
    int w;

    pkt[n++] = 0x00716572;                  // "req\0"
    pkt[n++] = 20 + kvlen;
    pkt[n++] = msg;
    pkt[n++] = 0;                           // active set
    pkt[n++] = type;
    pkt[n++] = id;
    pkt[n++] = kvlen;
    for (i = 0; i < kvlen / 4; i++)
        pkt[n++] = kv[i];
    hdr[0] = CMD_TX_DATA | ((ULONG)g_lcid << 16);
    hdr[1] = 0;                             // intentless: iid 0
    hdr[2] = n * 4;                         // chunk size
    hdr[3] = 0;                             // left
    g_ack_id = 0;
    g_ack_err = 0;
    if (Tx(hdr, 16, pkt, n * 4))
        return -1;
    for (w = 0; w < 200; w++) {
        RxPoll();
        if (g_ack_id == msg)
            return g_ack_err ? -2 : 0;
        SleepMs(5);
    }
    Log("request %lu: no ack", (unsigned long)msg);
    return -3;
}

#define RES_LDOA    0x616f646c
#define KEY_SWEN    0x6e657773
#define KEY_UV      0x00007675

static int Ldo(ULONG n, ULONG on, ULONG uv)
{
    ULONG kv[6];
    ULONG k = 0;
    int r;

    if (on && uv) {
        kv[k++] = KEY_UV;
        kv[k++] = 4;
        kv[k++] = uv;
    }
    kv[k++] = KEY_SWEN;
    kv[k++] = 4;
    kv[k++] = on;
    r = RpmWrite(RES_LDOA, n, kv, k * 4);
    Log("ldoa%lu %s %lu uV -> %d%s%s", (unsigned long)n, on ? "on" : "off", (unsigned long)uv, r, r == -2 ? " " : "", r == -2 ? g_err : "");
    return r;
}

// =====================================================================================================================================
// SPMI read only (L17..L23 have apps observer channels): STATUS 0x08 (bit7 VREG_OK)

#define SPMI_CORE_PA 0x01c40000
#define SPMI_OBS_PA  0x03e00000

static void SpmiLdoStatus(const char *tag)
{
    volatile UCHAR *core = Map(SPMI_CORE_PA, 0x2000);
    int n;
    if (!core)
        return;
    for (n = 17; n <= 23; n++) {
        USHORT base = (USHORT)(0x4000 + 0x100 * (n - 1)), ppid = (USHORT)(base >> 8);
        ULONG a;
        for (a = 0; 0x900 + 4 * a < 0x1100; a++)
            if (((Rd(core, 0x900 + 4 * a) >> 8) & 0xfff) == ppid)
                break;
        if (0x900 + 4 * a >= 0x1100)
            continue;
        {
            volatile UCHAR *ch = Map(SPMI_OBS_PA + 0x80 * a, 0x80);
            int v[2], k;
            USHORT regs[2] = { 0x08, 0x46 };
            if (!ch)
                continue;
            for (k = 0; k < 2; k++) {
                ULONG st = 0;
                int i;
                Wr(ch, 0x00, (1u << 27) | (((ULONG)regs[k] & 0xffu) << 4));
                for (i = 0; i < 2000; i++) {
                    st = Rd(ch, 0x08);
                    if (st & 1)
                        break;
                }
                v[k] = (st & 0xf) == 1 ? (int)(Rd(ch, 0x18) & 0xff) : -1;
            }
            Log("%s L%d status %02x en %02x", tag, n, v[0] & 0xff, v[1] & 0xff);
            MmUnmapIoSpace((PVOID)ch, 0x80);
        }
    }
    MmUnmapIoSpace((PVOID)core, 0x2000);
}

// =====================================================================================================================================

#ifndef RPM_NO_ENTRY
static int g_bt_on, g_open;

static VOID Unload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_bt_on) {
        Ldo(23, 0, 0);
        Ldo(17, 0, 0);
        Ldo(16, 0, 0);
        Ldo(9, 0, 0);
        SleepMs(20);
        SpmiLdoStatus("after off:");
    }
    if (g_open) {
        TxCmd(CMD_CLOSE, g_lcid, 0);
        if (WaitFor((volatile int *)&g_close_ack, 1000))
            Log("close: no CLOSE_ACK");
        else
            Log("channel closed");
    }
    Log("=== unload");
}

static int EndsWith(PUNICODE_STRING p, const char *s)
{
    USHORT n = p->Length / sizeof(WCHAR), m = 0, i;

    while (s[m])
        m++;
    if (n < m)
        return 0;
    for (i = 0; i < m; i++)
        if ((p->Buffer[n - m + i] | 0x20) != s[i])
            return 0;
    return 1;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    int bt = RegistryPath && RegistryPath->Buffer && EndsWith(RegistryPath, "rpmb");

    DriverObject->DriverUnload = Unload;
    Log("=== j606f rpm start, mode %s", bt ? "handshake + BT LDOs" : "handshake only");
    SpmiLdoStatus("before:");
    if (GlinkOpen()) {
        Log("glink open failed");
        return STATUS_SUCCESS;
    }
    g_open = 1;
    if (bt) {
        int ok = 1;
        ok &= Ldo(9, 1, 1800000) == 0;
        ok &= Ldo(16, 1, 1704000) == 0;
        ok &= Ldo(17, 1, 1152000) == 0;
        ok &= Ldo(23, 1, 3200000) == 0;
        g_bt_on = 1;
        SleepMs(20);
        SpmiLdoStatus(ok ? "after on (all acked):" : "after on (some failed):");
    }
    Log("=== init done");
    return STATUS_SUCCESS;
}
#endif // RPM_NO_ENTRY
