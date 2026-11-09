// J606F Wi-Fi steps C-E: the APPS-side QRTR services the modem needs before it starts the WLAN user PD, as the Linux userspace daemons:
//  - rmtfs (linux-msm/rmtfs): QMI service 14 v1 inst 0, our port 1. EFS images modem_fs1/fs2/fsg/fsc from C:\j606f\efs\ (copies made in pmOS
//    from this tablet) are kept in RAM; the modem's writes go only to these RAM copies (never to the flash). Shared buffer = rmtfs_mem
//    0x89b01000 + 2 MiB (DT), assigned to MSS_MSA + NAV + HLOS (RW) with the hypervisor MP_ASSIGN call as qcom_rmtfs_mem.c does.
//  - servreg locator (linux-msm/pd-mapper): service 64 v0x101 inst 0, port 2, GET_DOMAIN_LIST from the stock .jsn files (built in).
//  - TFTP (linux-msm/tqftpserv): service 4096 v1 inst 0, port 3; RRQ of any path ending in wlanmdsp.mbn -> C:\j606f\drv\wlanmdsp.mbn,
//    everything else -> ENOENT; WRQ (modem's readwrite files) is accepted and discarded. blksize capped to 4096, window 1.
// QMI wire format as libqrtr qmi.c: header {u8 type, u16 txn, u16 msg_id, u16 len}, TLVs {u8 type, u16 len, value}.
#define RMTFS_PA        0x89b01000u
#define RMTFS_SIZE      0x00200000u
#define SVC_TAG         0x63766d73

typedef struct { LARGE_INTEGER AllocationSize, EndOfFile; ULONG NumberOfLinks; BOOLEAN DeletePending, Directory; } FILE_STANDARD_INFORMATION;
#define FileStandardInformation 5
NTSTATUS ZwQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, int FileInformationClass);

static volatile UCHAR *g_rmem;
static UCHAR *g_efs[4];
static ULONG g_efs_len[4];
static const char *const g_efs_name[4] = { "/boot/modem_fs1", "/boot/modem_fs2", "/boot/modem_fsc", "/boot/modem_fsg" };
static const WCHAR *const g_efs_file[4] = { L"\\??\\C:\\j606f\\efs\\modem_fs1", L"\\??\\C:\\j606f\\efs\\modem_fs2",
                                            L"\\??\\C:\\j606f\\efs\\modem_fsc", L"\\??\\C:\\j606f\\efs\\modem_fsg" };
static ULONG g_efs_rd, g_efs_wr;

static UCHAR *LoadWhole(const WCHAR *path, ULONG *len, ULONG max)
{
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES a;
    IO_STATUS_BLOCK io;
    HANDLE f;
    FILE_STANDARD_INFORMATION si;
    UCHAR *b = NULL;

    *len = 0;
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&a, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&f, FILE_READ_DATA | SYNCHRONIZE, &a, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        return NULL;
    if (NT_SUCCESS(ZwQueryInformationFile(f, &io, &si, sizeof(si), FileStandardInformation)) && si.EndOfFile.QuadPart > 0 &&
        si.EndOfFile.QuadPart <= max) {
        ULONG n = (ULONG)si.EndOfFile.QuadPart;
        b = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, n, SVC_TAG);
        if (b && ReadAt(f, 0, b, n) == 0) {
            *len = n;
        } else if (b) {
            ExFreePoolWithTag(b, SVC_TAG);
            b = NULL;
        }
    }
    ZwClose(f);
    return b;
}

// hypervisor memory assign (qcom_scm_assign_mem): svc MP 0x0c cmd 0x16, args 7 (RO,VAL,RO,VAL,RO,VAL,VAL); args 3.. go indirect via x5
static long long ScmAssign(ULONG pa, ULONG size)
{
    PHYSICAL_ADDRESS lo, hi, bnd;
    ULONG *buf;
    U64 *ext, base, a6 = 0, r1 = 0, x0;
    int intr = 0;

    lo.QuadPart = 0;
    hi.QuadPart = 0xffffffffLL;
    bnd.QuadPart = 0;
    buf = (ULONG *)MmAllocateContiguousMemorySpecifyCache(4096, lo, hi, bnd, MmNonCached);
    if (!buf)
        return -100;
    for (r1 = 0; r1 < 1024; r1++)
        buf[r1] = 0;
    base = (U64)MmGetPhysicalAddress(buf).QuadPart;
    buf[0] = 3;                                         // src vmids: HLOS              @ +0   (4 B)
    *(U64 *)(buf + 16) = pa;                            // mem_to_map {addr, size}      @ +64  (16 B)
    *(U64 *)(buf + 18) = size;
    buf[32] = 0x0f; buf[33] = 6;                        // dest {vmid, perm, ctx, ctx_size, pad} @ +128, 24 B each: MSS_MSA RW
    buf[38] = 0x2b; buf[39] = 6;                        //   NAV RW
    buf[44] = 3;    buf[45] = 6;                        //   HLOS RW
    ext = (U64 *)(buf + 256);                           // indirect args @ +1024: src_sz, dest, dest_sz, 0
    ext[0] = 4;
    ext[1] = base + 128;
    ext[2] = 3 * 24;
    ext[3] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    {
        register U64 rx0 __asm__("x0") = 0x42000c16ull;
        register U64 rx1 __asm__("x1") = 0x1117;        // QCOM_SCM_ARGS(7, RO, VAL, RO, VAL, RO, VAL, VAL)
        register U64 rx2 __asm__("x2") = base + 64;
        register U64 rx3 __asm__("x3") = 16;
        register U64 rx4 __asm__("x4") = base;
        register U64 rx5 __asm__("x5") = base + 1024;
        register U64 rx6 __asm__("x6") = 0;
        register U64 rx7 __asm__("x7") = 0;
        __asm__ volatile("smc #0" : "+r"(rx0), "+r"(rx1), "+r"(rx2), "+r"(rx3), "+r"(rx4), "+r"(rx5), "+r"(rx6), "+r"(rx7)
                         : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        x0 = rx0;
        r1 = rx1;
        a6 = rx6;
    }
    while (x0 == 1 && intr < 1000000) {                 // QCOM_SCM_INTERRUPTED: resume with x6 from TZ (as Pil)
        register U64 rx0 __asm__("x0") = 1;
        register U64 rx1 __asm__("x1") = 0x1117;
        register U64 rx2 __asm__("x2") = base + 64;
        register U64 rx3 __asm__("x3") = 16;
        register U64 rx4 __asm__("x4") = base;
        register U64 rx5 __asm__("x5") = base + 1024;
        register U64 rx6 __asm__("x6") = a6;
        register U64 rx7 __asm__("x7") = 0;
        intr++;
        __asm__ volatile("smc #0" : "+r"(rx0), "+r"(rx1), "+r"(rx2), "+r"(rx3), "+r"(rx4), "+r"(rx5), "+r"(rx6), "+r"(rx7)
                         : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        x0 = rx0;
        r1 = rx1;
        a6 = rx6;
    }
    MmFreeContiguousMemory(buf);
    Log("svc: hyp assign %08lx+%lx -> MSS_MSA,NAV,HLOS RW: x0 %lld res %llu (%d resumes)", (unsigned long)pa, (unsigned long)size,
        (long long)x0, r1, intr);
    return x0 ? (long long)x0 : (long long)r1;
}

// ---- QMI helpers ----
typedef struct { UCHAR b[600]; ULONG n; } QMSG;

static void QmHdr(QMSG *m, UCHAR type, USHORT txn, USHORT id)
{
    m->b[0] = type;
    m->b[1] = (UCHAR)txn; m->b[2] = (UCHAR)(txn >> 8);
    m->b[3] = (UCHAR)id;  m->b[4] = (UCHAR)(id >> 8);
    m->n = 7;
}

static void QmTlv(QMSG *m, UCHAR t, const void *v, ULONG len)
{
    ULONG i;

    if (m->n + 3 + len > sizeof(m->b))
        return;
    m->b[m->n++] = t;
    m->b[m->n++] = (UCHAR)len;
    m->b[m->n++] = (UCHAR)(len >> 8);
    for (i = 0; i < len; i++)
        m->b[m->n++] = ((const UCHAR *)v)[i];
}

static void QmResult(QMSG *m, USHORT result, USHORT error)
{
    UCHAR r[4] = { (UCHAR)result, (UCHAR)(result >> 8), (UCHAR)error, (UCHAR)(error >> 8) };
    QmTlv(m, 2, r, 4);
}

static ULONG QmEnd(QMSG *m)
{
    ULONG l = m->n - 7;
    m->b[5] = (UCHAR)l;
    m->b[6] = (UCHAR)(l >> 8);
    return m->n;
}

// find TLV t in a QMI message body (after the 7-byte header); returns pointer and length
static const UCHAR *QmFind(const UCHAR *pl, ULONG plen, UCHAR t, ULONG *len)
{
    ULONG o = 7;

    while (o + 3 <= plen) {
        ULONG l = pl[o + 1] | (pl[o + 2] << 8);
        if (o + 3 + l > plen)
            return NULL;
        if (pl[o] == t) {
            *len = l;
            return pl + o + 3;
        }
        o += 3 + l;
    }
    return NULL;
}

static ULONG Le32p(const UCHAR *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24); }

// ---- rmtfs ----
static void RmtfsRx(ULONG node, ULONG port, const UCHAR *pl, ULONG plen)
{
    USHORT txn = pl[1] | (pl[2] << 8), id = pl[3] | (pl[4] << 8);
    QMSG m;
    const UCHAR *v;
    ULONG l, i;

    QmHdr(&m, 2, txn, id);
    switch (id) {
    case 1: {                                           // OPEN path -> caller_id
        char path[64];
        int idx = -1;
        v = QmFind(pl, plen, 1, &l);
        for (i = 0; v && i < l && i < sizeof(path) - 1; i++)
            path[i] = (char)v[i];
        path[v ? i : 0] = 0;
        for (i = 0; i < 4; i++) {
            const char *a = path, *b = g_efs_name[i];
            while (*a && *a == *b) { a++; b++; }
            if (!*a && !*b)
                idx = (int)i;
        }
        if (idx >= 0 && g_efs[idx]) {
            ULONG cid = idx + 1;
            QmResult(&m, 0, 0);
            QmTlv(&m, 0x10, &cid, 4);
        } else {
            QmResult(&m, 1, 1);
        }
        Log("rmtfs: open %s -> %d", path, idx >= 0 && g_efs[idx] ? idx + 1 : -1);
        break;
    }
    case 2:                                             // CLOSE
        QmResult(&m, 0, 0);
        Log("rmtfs: close");
        break;
    case 3: {                                           // RW_IOVEC caller_id, direction, iovec[], force
        const UCHAR *c = QmFind(pl, plen, 1, &l), *d = QmFind(pl, plen, 2, &i), *io = QmFind(pl, plen, 3, &l);
        ULONG cid = c ? Le32p(c) : 0, n = io ? io[0] : 0, k, ok = 1, wr = d ? d[0] : 0, secs = 0;
        UCHAR *img = (cid >= 1 && cid <= 4) ? g_efs[cid - 1] : NULL;
        ULONG ilen = img ? g_efs_len[cid - 1] : 0;
        for (k = 0; img && g_rmem && k < n && 1 + 12 * (k + 1) <= l; k++) {
            ULONG sec = Le32p(io + 1 + 12 * k), phys = Le32p(io + 5 + 12 * k), cnt = Le32p(io + 9 + 12 * k);
            ULONG fo = sec * 512, mo = phys - RMTFS_PA, bytes = cnt * 512;
            if (phys < RMTFS_PA || mo + bytes > RMTFS_SIZE || bytes > RMTFS_SIZE) { ok = 0; break; }
            if (wr) {                                   // modem -> RAM copy only
                for (i = 0; i < bytes; i++)
                    if (fo + i < ilen)
                        img[fo + i] = g_rmem[mo + i];
            } else {
                for (i = 0; i < bytes; i++)
                    g_rmem[mo + i] = fo + i < ilen ? img[fo + i] : 0;
            }
            secs += cnt;
        }
        if (!img || !g_rmem)
            ok = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        if (wr) g_efs_wr += secs; else g_efs_rd += secs;
        QmResult(&m, ok ? 0 : 1, ok ? 0 : 1);
        if (g_efs_rd + g_efs_wr < 20000 || !ok)
            Log("rmtfs: %s caller %lu %lu iovec %lu sectors -> %s (total rd %lu wr %lu)", wr ? "write" : "read", (unsigned long)cid,
                (unsigned long)n, (unsigned long)secs, ok ? "ok" : "FAIL", (unsigned long)g_efs_rd, (unsigned long)g_efs_wr);
        break;
    }
    case 4: {                                           // ALLOC_BUFF -> rmtfs_mem base
        const UCHAR *s = QmFind(pl, plen, 2, &l);
        ULONG sz = s ? Le32p(s) : 0;
        if (g_rmem && sz <= RMTFS_SIZE) {
            U64 a = RMTFS_PA;
            QmResult(&m, 0, 0);
            QmTlv(&m, 0x10, &a, 8);
        } else {
            QmResult(&m, 1, 1);
        }
        Log("rmtfs: alloc %lu -> %s", (unsigned long)sz, g_rmem && sz <= RMTFS_SIZE ? "0x89b01000" : "FAIL");
        break;
    }
    case 5: {                                           // GET_DEV_ERROR -> 0
        UCHAR st = 0;
        QmResult(&m, 0, 0);
        QmTlv(&m, 0x10, &st, 1);
        break;
    }
    default:
        Log("rmtfs: unknown msg %u", id);
        QmResult(&m, 1, 2);
        break;
    }
    QrtrSendData(1, node, port, m.b, QmEnd(&m));
}

// ---- servreg locator (pd-mapper): stock jsn files of this tablet ----
typedef struct { const char *service, *domain; ULONG instance; } PDMAP;
static const PDMAP g_pdmap[] = {
    { "tms/servreg", "msm/modem/root_pd", 180 },  { "gps/gps_service", "msm/modem/root_pd", 180 },
    { "kernel/elf_loader", "msm/modem/wlan_pd", 180 }, { "tms/servreg", "msm/modem/wlan_pd", 180 }, { "wlan/fw", "msm/modem/wlan_pd", 180 },
    { "tms/servreg", "msm/adsp/root_pd", 74 },    { "tms/servreg", "msm/adsp/sensor_pd", 74 },
    { "tms/servreg", "msm/adsp/audio_pd", 74 },   { "avs/audio", "msm/adsp/audio_pd", 74 },
    { "tms/servreg", "msm/cdsp/root_pd", 76 },
};

static void ServregRx(ULONG node, ULONG port, const UCHAR *pl, ULONG plen)
{
    USHORT txn = pl[1] | (pl[2] << 8), id = pl[3] | (pl[4] << 8);
    QMSG m;
    const UCHAR *v;
    ULONG l, i, k, cnt = 0;
    char name[80];
    UCHAR list[400];
    ULONG ln = 1;
    USHORT tot, rev = 1;

    QmHdr(&m, 2, txn, id);
    if (id == 0x24) {                        // SERVREG_LOC_PFR: a protection domain crashed; TLV 1 = service, TLV 2 = reason
        char svc[64], why[200];
        v = QmFind(pl, plen, 1, &l);
        for (i = 0; v && i < l && i < sizeof(svc) - 1; i++)
            svc[i] = (v[i] >= 32 && v[i] < 127) ? (char)v[i] : '?';
        svc[v ? i : 0] = 0;
        v = QmFind(pl, plen, 2, &l);
        for (i = 0; v && i < l && i < sizeof(why) - 1; i++)
            why[i] = (v[i] >= 32 && v[i] < 127) ? (char)v[i] : '?';
        why[v ? i : 0] = 0;
        Log("servreg: PD CRASH report: service '%s' reason '%s'", svc, why);
    }
    if (id != 0x20 && id != 0x21) {          // GET_DOMAIN_LIST: the modem sends 0x21 (pd-mapper SERVREG_LOC_GET_DOMAIN_LIST)
        Log("servreg: msg %u -> ok", id);
        QmResult(&m, 0, 0);
        QrtrSendData(2, node, port, m.b, QmEnd(&m));
        return;
    }
    v = QmFind(pl, plen, 1, &l);
    for (i = 0; v && i < l && i < sizeof(name) - 1; i++)
        name[i] = (char)v[i];
    name[v ? i : 0] = 0;
    for (k = 0; k < sizeof(g_pdmap) / sizeof(g_pdmap[0]); k++) {
        const char *a = name, *b = g_pdmap[k].service, *d = g_pdmap[k].domain;
        ULONG dl = 0;
        while (*a && *a == *b) { a++; b++; }
        if (*a || *b)
            continue;
        while (d[dl])
            dl++;
        if (ln + 1 + dl + 9 > sizeof(list))
            break;
        list[ln++] = (UCHAR)dl;
        for (i = 0; i < dl; i++)
            list[ln++] = (UCHAR)d[i];
        for (i = 0; i < 4; i++)
            list[ln++] = (UCHAR)(g_pdmap[k].instance >> (8 * i));
        list[ln++] = 0;                                 // service_data_valid
        for (i = 0; i < 4; i++)
            list[ln++] = 0;                             // service_data
        cnt++;
    }
    list[0] = (UCHAR)cnt;
    tot = (USHORT)cnt;
    QmResult(&m, 0, 0);
    QmTlv(&m, 0x10, &tot, 2);
    QmTlv(&m, 0x11, &rev, 2);
    if (cnt)
        QmTlv(&m, 0x12, list, ln);
    Log("servreg: get_domain_list '%s' -> %lu domains", name, (unsigned long)cnt);
    QrtrSendData(2, node, port, m.b, QmEnd(&m));
}

// ---- TFTP ----
typedef struct { int used, wr; ULONG node, port, myport, blksize, rsize, seek, wsize; HANDLE f; ULONG fsize; LONGLONG t; } TFTPC;
static TFTPC g_tf[4];
static ULONG g_tf_nextport = 16;
static UCHAR g_tfbuf[4096 + 8];

static ULONG ParseNum(const char *s)
{
    ULONG v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (ULONG)(*s++ - '0');
    return v;
}

static ULONG PutStr(UCHAR *d, const char *s)
{
    ULONG n = 0;
    while (s[n]) { d[n] = (UCHAR)s[n]; n++; }
    d[n++] = 0;
    return n;
}

static ULONG PutNum(UCHAR *d, ULONG v)
{
    char t[12];
    int k = 0;
    ULONG n = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k)
        d[n++] = (UCHAR)t[--k];
    d[n++] = 0;
    return n;
}

static void TftpErr(ULONG myport, ULONG node, ULONG port, int code, const char *msg)
{
    UCHAR b[64];
    ULONG n = 4;
    b[0] = 0; b[1] = 5; b[2] = 0; b[3] = (UCHAR)code;
    n += PutStr(b + 4, msg);
    QrtrSendData(myport, node, port, b, n);
}

static int TftpData(TFTPC *c, ULONG block)
{
    ULONG off = c->seek + (block - 1) * c->blksize, n = 0;
    IO_STATUS_BLOCK io;
    LARGE_INTEGER o;

    if (c->rsize && (block - 1) * c->blksize >= c->rsize)
        return -1;
    if (!c->rsize && off > c->fsize)
        return -1;
    if (off < c->fsize) {
        n = c->fsize - off;
        if (n > c->blksize)
            n = c->blksize;
        if (c->rsize && (block - 1) * c->blksize + n > c->rsize)
            n = c->rsize - (block - 1) * c->blksize;
        o.QuadPart = off;
        if (!NT_SUCCESS(ZwReadFile(c->f, NULL, NULL, NULL, &io, g_tfbuf + 4, n, &o, NULL)) || io.Information != n)
            n = 0;
    }
    g_tfbuf[0] = 0; g_tfbuf[1] = 3; g_tfbuf[2] = (UCHAR)(block >> 8); g_tfbuf[3] = (UCHAR)block;
    QrtrSendData(c->myport, c->node, c->port, g_tfbuf, 4 + n);
    c->t = NowUs();
    return (int)n;
}

static void TftpClose(TFTPC *c)
{
    if (c->f)
        ZwClose(c->f);
    c->f = NULL;
    c->used = 0;
}

static void TftpRequest(ULONG node, ULONG port, const UCHAR *pl, ULONG plen)
{
    const char *p = (const char *)pl + 2, *end = (const char *)pl + plen, *fname, *mode;
    ULONG blksize = 512, tsize_req = 0, rsize = 0, seek = 0, i, wsize = 0, timeoutms = 0, has_opt = 0, n;
    int wr = pl[1] == 2, slot = -1;
    TFTPC *c;
    UCHAR o[256];
    ULONG on = 2;

    fname = p;
    while (p < end && *p) p++;
    if (p >= end) return;
    p++;
    mode = p;
    while (p < end && *p) p++;
    if (p >= end) return;
    p++;
    (void)mode;
    while (p < end) {                                   // options: name\0value\0
        const char *k = p, *v;
        while (p < end && *p) p++;
        if (p >= end) break;
        v = ++p;
        while (p < end && *p) p++;
        if (p >= end) break;
        p++;
        has_opt = 1;
        if (k[0] == 'b') blksize = ParseNum(v);
        else if (k[0] == 't' && k[1] == 's') tsize_req = 1;
        else if (k[0] == 't' && k[1] == 'i') timeoutms = ParseNum(v);
        else if (k[0] == 'r') rsize = ParseNum(v);
        else if (k[0] == 's') seek = ParseNum(v);
        else if (k[0] == 'w') wsize = ParseNum(v);
    }
    if (blksize > 4096) blksize = 4096;
    if (blksize < 8) blksize = 512;
    for (i = 0; i < 4; i++)
        if (g_tf[i].used && g_tf[i].node == node && g_tf[i].port == port)
            return;                                     // retransmitted request: the first answer is on its way
    for (i = 0; i < 4; i++)
        if (!g_tf[i].used) { slot = (int)i; break; }
    if (slot < 0) {                                     // evict the oldest transfer
        for (i = 0; i < 4; i++)
            if (slot < 0 || g_tf[i].t < g_tf[slot].t)
                slot = (int)i;
        TftpClose(&g_tf[slot]);
    }
    {
        char fn[100];
        for (i = 0; fname[i] && i < sizeof(fn) - 1; i++)
            fn[i] = fname[i];
        fn[i] = 0;
        Log("tftp: %s '%s' blksize %lu rsize %lu seek %lu wsize %lu from %lu:%lu", wr ? "WRQ" : "RRQ", fn, (unsigned long)blksize,
            (unsigned long)rsize, (unsigned long)seek, (unsigned long)wsize, (unsigned long)node, (unsigned long)port);
    }
    if (slot < 0) {
        TftpErr(3, node, port, 0, "busy");
        return;
    }
    c = &g_tf[slot];
    c->used = 1; c->wr = wr; c->node = node; c->port = port; c->myport = g_tf_nextport++; c->blksize = blksize;
    c->rsize = rsize; c->seek = seek; c->f = NULL; c->fsize = 0; c->t = NowUs();
    c->wsize = wsize ? (wsize > 10 ? 10 : wsize) : 1;
    if (!wr) {
        ULONG fl = 0;
        const char *s = fname, *want = "wlanmdsp.mbn";
        while (s[fl]) fl++;
        if (fl >= 12) {
            const char *t = s + fl - 12;
            for (i = 0; i < 12 && t[i] == want[i]; i++)
                ;
            if (i == 12) {
                UNICODE_STRING u;
                OBJECT_ATTRIBUTES a;
                IO_STATUS_BLOCK io;
                FILE_STANDARD_INFORMATION si;
                RtlInitUnicodeString(&u, L"\\??\\C:\\j606f\\drv\\wlanmdsp.mbn");
                InitializeObjectAttributes(&a, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                if (NT_SUCCESS(ZwCreateFile(&c->f, FILE_READ_DATA | SYNCHRONIZE, &a, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                            FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0)) &&
                    NT_SUCCESS(ZwQueryInformationFile(c->f, &io, &si, sizeof(si), FileStandardInformation)))
                    c->fsize = (ULONG)si.EndOfFile.QuadPart;
            }
        }
        if (!c->f) {
            TftpErr(c->myport, node, port, 1, "file not found");
            TftpClose(c);
            return;
        }
    }
    if (has_opt) {                                      // OACK echoes what we accept
        o[0] = 0; o[1] = 6;
        on += PutStr(o + on, "blksize");
        on += PutNum(o + on, blksize);
        if (timeoutms) { on += PutStr(o + on, "timeoutms"); on += PutNum(o + on, timeoutms); }
        if (tsize_req && !wr) { on += PutStr(o + on, "tsize"); on += PutNum(o + on, c->fsize); }
        if (wsize) { on += PutStr(o + on, "wsize"); on += PutNum(o + on, c->wsize); }
        if (rsize) { on += PutStr(o + on, "rsize"); on += PutNum(o + on, rsize); }
        if (seek) { on += PutStr(o + on, "seek"); on += PutNum(o + on, seek); }
        n = QrtrSendData(c->myport, node, port, o, on);
        (void)n;
    } else if (wr) {
        UCHAR ack[4] = { 0, 4, 0, 0 };
        QrtrSendData(c->myport, node, port, ack, 4);
    } else {
        TftpData(c, 1);
    }
}

static void TftpTransfer(TFTPC *c, const UCHAR *pl, ULONG plen)
{
    ULONG op = (pl[0] << 8) | pl[1], blk = plen >= 4 ? ((pl[2] << 8) | pl[3]) : 0;

    if (op == 5) {                                      // ERROR (9 = end of transfer)
        Log("tftp: port %lu: error %lu from modem (end)", (unsigned long)c->myport, (unsigned long)blk);
        TftpClose(c);
    } else if (!c->wr && op == 4) {                     // ACK n -> DATA n+1 (window 1)
        ULONG k;
        if ((c->rsize && blk * c->blksize >= c->rsize) || (!c->rsize && c->seek + blk * c->blksize > c->fsize)) {
            TftpClose(c);
            return;
        }
        for (k = 1; k <= c->wsize; k++)                 // window: blocks blk+1 .. blk+wsize
            if (TftpData(c, blk + k) < (int)c->blksize)
                break;
    } else if (c->wr && op == 3) {                      // DATA from modem -> discard, ACK
        UCHAR ack[4] = { 0, 4, pl[2], pl[3] };
        QrtrSendData(c->myport, c->node, c->port, ack, 4);
        if (plen - 4 < c->blksize)
            TftpClose(c);
    } else {
        Log("tftp: port %lu: op %lu blk %lu", (unsigned long)c->myport, (unsigned long)op, (unsigned long)blk);
    }
}

#include "wlan.inc"
#include "wlfw.inc"

// ---- dispatch from QrtrRx (DATA to our node) ----
static void SvcRx(ULONG src_node, ULONG src_port, ULONG dst_port, const UCHAR *pl, ULONG plen)
{
    ULONG i;

    if (dst_port == 1 && plen >= 7)
        RmtfsRx(src_node, src_port, pl, plen);
    else if (dst_port == 2 && plen >= 7)
        ServregRx(src_node, src_port, pl, plen);
    else if (dst_port == WF_PORT)
        WfRx(src_node, src_port, pl, plen);
    else if (dst_port == 3 && plen >= 4 && pl[0] == 0 && (pl[1] == 1 || pl[1] == 2))
        TftpRequest(src_node, src_port, pl, plen);
    else {
        for (i = 0; i < 4; i++)
            if (g_tf[i].used && g_tf[i].myport == dst_port && plen >= 4) {
                TftpTransfer(&g_tf[i], pl, plen);
                return;
            }
        Log("svc: data for port %lu (%lu B) from %lu:%lu ignored", (unsigned long)dst_port, (unsigned long)plen, (unsigned long)src_node,
            (unsigned long)src_port);
    }
}

static int SvcInit(void)
{
    ULONG i;

    for (i = 0; i < 4; i++) {
        g_efs[i] = LoadWhole(g_efs_file[i], &g_efs_len[i], 4u << 20);
        Log("svc: efs %s: %lu B", g_efs_name[i], (unsigned long)g_efs_len[i]);
    }
    if (ScmAssign(RMTFS_PA, RMTFS_SIZE) == 0)
        g_rmem = Map(RMTFS_PA, RMTFS_SIZE);
    Log("svc: rmtfs_mem %s", g_rmem ? "mapped" : "NOT available");
    WfInit();
    return 0;
}
