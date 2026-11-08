// J606F: ADSP (LPASS Q6, PAS id 1) start through TrustZone, following Linux qcom_q6v5_pas + qcom_mdt_loader for "qcom,sm6115-adsp-pas".
//   1. proxy votes over RPM: LPI_CX (rwlc 0) and LPI_MX (rwlm 0): swen 1, vlvl 416 (TURBO_NO_CPR = rpmpd sm6115 max_state);
//   2. PAS_INIT_IMAGE(1, metadata): ELF header + phdrs (phdr[0].p_filesz) + hash segment, physically contiguous buffer;
//   3. PAS_MEM_SETUP(1, 0x53800000, image span) (relocatable image);
//   4. loadable segments copied to the carveout at p_paddr - min_paddr, tail (memsz > filesz) zeroed;
//   5. PAS_AUTH_AND_RESET(1);
//   6. wait up to 10 s for the ADSP's SMP2P "ready" bit (item 443 in the APPS/ADSP SMEM partition: bit 1 ready, bit 2 handover); read only.
// Same path for the modem (MPSS, PAS id 4, needed for Wi-Fi: WLAN firmware runs on it): table g_mpss, C:\j606f\drv\mpss.arm + modem.mbn.
// One shot: runs only if C:\j606f\drv\adsp.arm exists (opened delete-on-close first, so it never repeats), checked every 5 s once the
// 60 s start window of btbus is over. Firmware: C:\j606f\drv\adsp.mbn (Lenovo blob, private). Nothing is written to flash.
// Included by btbus.c: uses its RPM GLINK channel (RpmWrite), Log, Map, SleepMs, FileOp.

PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID BaseAddress);
PVOID MmAllocateContiguousMemorySpecifyCache(SIZE_T NumberOfBytes, PHYSICAL_ADDRESS Lowest, PHYSICAL_ADDRESS Highest,
                                             PHYSICAL_ADDRESS Boundary, MEMORY_CACHING_TYPE CacheType);
VOID MmFreeContiguousMemory(PVOID BaseAddress);

#define PAGE_WRITECOMBINE_  0x400
#define SMEM_PA             0x46000000u
#define SMEM_SIZE           0x00200000u
#define RES_RWLC            0x636c7772
#define RES_RWLM            0x6d6c7772
#define KEY_LEVEL_          0x6c766c76
#define LEVEL_MAX           416
#define ADSP_TAG            0x70736461
#define SMP2P_MAGIC         0x504d5324          // "$SMP"
#define CHUNK               0x10000
#define RES_RWCX            0x78637772          // "rwcx": VDDCX (rpmpd SM6115_VDDCX)

// Remote processors started like Linux qcom_q6v5_pas (DT sm6115.dtsi: remoteproc_adsp / remoteproc_mpss, smp2p-adsp / smp2p-mpss)
typedef struct {
    const char *name;
    int pas_id;
    ULONG mem_pa, mem_size;
    const WCHAR *fw, *arm;
    ULONG host;                                     // SMEM partition APPS(0)/<host>
    ULONG smp2p_in, smp2p_out, kick_bit;
    ULONG votes[2];                                 // RPM level resources voted to LEVEL_MAX (proxy votes)
    const char *vote_names[2];
} RPROC;

static const RPROC g_adsp = { "adsp", 1, 0x53800000u, 0x02800000u, L"\\??\\C:\\j606f\\drv\\adsp.mbn", L"\\??\\C:\\j606f\\drv\\adsp.arm",
                              2, 443, 429, 10, { RES_RWLC, RES_RWLM }, { "lpi_cx", "lpi_mx" } };
static const RPROC g_mpss = { "mpss", 4, 0x4ab00000u, 0x06900000u, L"\\??\\C:\\j606f\\drv\\modem.mbn", L"\\??\\C:\\j606f\\drv\\mpss.arm",
                              1, 435, 428, 14, { RES_RWCX, 0 }, { "cx", NULL } };
static const RPROC *g_rp = &g_adsp;

typedef unsigned long long U64;

static U64 Smc5(U64 a0, U64 a1, U64 a2, U64 a3, U64 a4, U64 *r1, U64 *a6)
{
    register U64 x0 __asm__("x0") = a0;
    register U64 x1 __asm__("x1") = a1;
    register U64 x2 __asm__("x2") = a2;
    register U64 x3 __asm__("x3") = a3;
    register U64 x4 __asm__("x4") = a4;
    register U64 x5 __asm__("x5") = 0;
    register U64 x6 __asm__("x6") = *a6;
    register U64 x7 __asm__("x7") = 0;

    __asm__ volatile("smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :
                     : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    *r1 = x1;
    *a6 = x6;
    return x0;
}

// SCM std call, SMC64 SIP, svc PIL (2), as Linux qcom_scm-smc.c __scm_smc_do_quirk (ARM_SMCCC_QUIRK_QCOM_A6):
//   x6 = 0 on the first call; on QCOM_SCM_INTERRUPTED (x0 = 1) call again with x0 = 1 and x6 = the x6 TZ returned (its session
//   context), other args unchanged. V2_EBUSY (-12) -> retry after 30 ms.
//   run_54..59 resumed with x6 = 0 -> suspected cause of the CPU stuck in TZ (CLOCK_WATCHDOG_TIMEOUT 0x101).
static long long Pil(int cmd, U64 arginfo, U64 a, U64 b, U64 c, U64 *res)
{
    U64 x0, a6;
    int busy = 0, intr = 0;
    U64 first_a6 = 0;

    LONGLONG t0 = NowUs();

    *res = 0;
    for (;;) {
        a6 = 0;
        x0 = Smc5(0x42000200ull | (unsigned)cmd, arginfo, a, b, c, res, &a6);
        while (x0 == 1 && intr < 1000000) {
            intr++;
            if (intr == 1)
                first_a6 = a6;
            x0 = Smc5(1, arginfo, a, b, c, res, &a6);
        }
        if ((long long)x0 == -12 && busy < 20) {
            busy++;
            SleepMs(30);
            continue;
        }
        break;
    }
    Log("adsp: pil cmd %d: %lld us, %d resumes (x6 %llx), %d busy retries", cmd, (long long)(NowUs() - t0), intr, first_a6, busy);
    return (long long)x0;
}

static int RpmLevel(ULONG type, const char *name)
{
    ULONG kv[3];
    int r1, r2;

    kv[0] = KEY_SWEN;
    kv[1] = 4;
    kv[2] = 1;
    r1 = RpmWrite(type, 0, kv, 12);
    kv[0] = KEY_LEVEL_;
    kv[2] = LEVEL_MAX;
    r2 = RpmWrite(type, 0, kv, 12);
    Log("adsp: rpm %s enable -> %d, level %d -> %d %s", name, r1, LEVEL_MAX, r2, (r1 == -2 || r2 == -2) ? g_err : "");
    return (r1 == 0 && r2 == 0) ? 0 : -1;
}

static ULONG ARd(const volatile UCHAR *p)
{
    return *(const volatile ULONG *)p;
}

// run_57: 0x101 inside PAS_INIT_IMAGE also with the default tick -> the CPU really hangs in TZ. Linux qcom_scm_bw_enable() votes the
// crypto engine path (sm6115 scm: interconnects MASTER_CRYPTO_CORE0 -> SLAVE_EBI_CH0) before every PAS call: without it the crypto core that
// checks the signature has no bus/clock and TZ hangs. RPM bus votes as icc-rpm: "bmas" mas_rpm_id 23 (crypto_c0), "bslv" slv_rpm_id 0 (ebi),
// key "bw", u64 bytes/s; plus the RPM CE clock ("ce", id 0, "KHz") as older kernels did (error from RPM is only logged).
#define RES_BMAS   0x73616d62
#define RES_BSLV   0x766c7362
#define RES_CE     0x00006563
#define KEY_BW     0x00007762
#define KEY_KHZ    0x007a484b

static int RpmBw(ULONG type, ULONG id, ULONGLONG bps, const char *name)
{
    ULONG kv[4];
    int r;

    kv[0] = KEY_BW;
    kv[1] = 8;
    kv[2] = (ULONG)bps;
    kv[3] = (ULONG)(bps >> 32);
    r = RpmWrite(type, id, kv, 16);
    Log("adsp: rpm %s bw %llu -> %d %s", name, bps, r, r == -2 ? g_err : "");
    return r;
}

static int CryptoVotes(void)
{
    ULONG kv[3];
    int r1, r2, r3;

    r1 = RpmBw(RES_BMAS, 23, 1000000000ull, "bmas 23 (crypto_c0)");
    r2 = RpmBw(RES_BSLV, 0, 1000000000ull, "bslv 0 (ebi)");
    kv[0] = KEY_KHZ;
    kv[1] = 4;
    kv[2] = 192000;
    r3 = RpmWrite(RES_CE, 0, kv, 12);
    Log("adsp: rpm ce clk 192000 kHz -> %d %s", r3, r3 == -2 ? g_err : "");
    return (r1 == 0 && r2 == 0) ? 0 : -1;
}

// SMEM, read only and only where Linux qcom_smem reads (run_52: a scan of all 2 MB reset the SoC, protected partitions):
// the TOC page (last 4 KB, "$TOC") and the private partition of hosts 0 (APPS) and 2 (ADSP), mapped by its own size.
static volatile UCHAR *g_part;
static ULONG g_part_size;

static int SmemOpenPart(void)
{
    volatile UCHAR *toc = Map(SMEM_PA + SMEM_SIZE - 0x1000, 0x1000);
    ULONG n, i, off = 0, size = 0;

    if (!toc)
        return -1;
    if (ARd(toc) != 0x434f5424) {                       // "$TOC"
        Log("adsp: smem toc magic %08lx -> no smem", (unsigned long)ARd(toc));
        MmUnmapIoSpace((PVOID)toc, 0x1000);
        return -1;
    }
    n = ARd(toc + 8);
    Log("adsp: smem toc version %lu, %lu partitions", (unsigned long)ARd(toc + 4), (unsigned long)n);
    for (i = 0; i < n && i < 64 && 32 + (i + 1) * 48 <= 0x1000; i++) {
        volatile UCHAR *e = toc + 32 + i * 48;
        ULONG h = ARd(e + 12), h0 = h & 0xffff, h1 = h >> 16;
        Log("adsp:   part %lu: off %06lx size %06lx hosts %lu/%lu", (unsigned long)i, (unsigned long)ARd(e), (unsigned long)ARd(e + 4),
            (unsigned long)h0, (unsigned long)h1);
        if ((h0 == 0 && h1 == g_rp->host) || (h0 == g_rp->host && h1 == 0)) {
            off = ARd(e);
            size = ARd(e + 4);
        }
    }
    MmUnmapIoSpace((PVOID)toc, 0x1000);
    if (!size || off + size > SMEM_SIZE - 0x1000 || size > 0x100000) {
        Log("adsp: no APPS/%s partition", g_rp->name);
        return -1;
    }
    g_part = Map(SMEM_PA + off, size);
    g_part_size = size;
    if (!g_part)
        return -1;
    if (ARd(g_part) != 0x54525024) {                    // "$PRT"
        Log("adsp: partition magic %08lx", (unsigned long)ARd(g_part));
        MmUnmapIoSpace((PVOID)g_part, size);
        g_part = NULL;
        return -1;
    }
    return 0;
}

// inbound SMP2P item (remote -> APPS): value of the "slave-kernel" entry (bit 0 fatal, 1 ready, 2 handover), ~0 if not there yet
static ULONG Smp2pRead(int verbose)
{
    ULONG p, end, i;

    if (!g_part)
        return ~0u;
    end = ARd(g_part + 12);                             // offset_free_uncached
    if (end > g_part_size)
        end = g_part_size;
    for (p = 32; p + 16 <= end;) {
        volatile UCHAR *e = g_part + p;
        ULONG w0 = ARd(e), sz = ARd(e + 4), padh = ARd(e + 8) >> 16;
        if ((w0 & 0xffff) != 0xa5a5)
            break;
        if ((w0 >> 16) == g_rp->smp2p_in && p + 16 + padh + 20 <= end) {
            volatile UCHAR *it = e + 16 + padh;
            ULONG val = ARd(it + 12) >> 16;
            if (ARd(it) != SMP2P_MAGIC)
                return ~0u;
            if (verbose)
                Log("adsp: smp2p %lu: ver/feat %08lx pid %lu->%lu entries %lu/%lu flags %08lx", (unsigned long)g_rp->smp2p_in, (unsigned long)ARd(it + 4), (unsigned long)(ARd(it + 8) & 0xffff),
                    (unsigned long)(ARd(it + 8) >> 16), (unsigned long)val, (unsigned long)(ARd(it + 12) & 0xffff), (unsigned long)ARd(it + 16));
            for (i = 0; verbose && i < val && i < 8 && p + 16 + padh + 40 + i * 20 <= end; i++) {
                volatile UCHAR *en = it + 20 + i * 20;
                char nm[17];
                int k;
                for (k = 0; k < 16; k++) {
                    UCHAR c = en[k];
                    nm[k] = (c >= 32 && c < 127) ? (char)c : (c ? '?' : 0);
                }
                nm[16] = 0;
                Log("adsp:   [%lu] %s = %08lx", (unsigned long)i, nm, (unsigned long)ARd(en + 16));
            }
            for (i = 0; i < val && i < 16 && p + 16 + padh + 40 + i * 20 <= end; i++) {     // q6v5 bits live in "slave-kernel"
                volatile UCHAR *en = it + 20 + i * 20;
                if (ARd(en) == 0x76616c73 && ARd(en + 4) == 0x656b2d65 && ARd(en + 8) == 0x6c656e72)    // "slave-kernel"
                    return ARd(en + 16);
            }
            return ~0u;
        }
        p += 16 + padh + sz;
    }
    return ~0u;
}

static void WrBytes(volatile UCHAR *dst, const UCHAR *src, ULONG n)
{
    ULONG i = 0;

    while (i < n && (((ULONG_PTR)(dst + i)) & 3))
        { dst[i] = src ? src[i] : 0; i++; }
    for (; i + 4 <= n; i += 4)
        *(volatile ULONG *)(dst + i) = src ? *(const ULONG *)(src + i) : 0;   // src: pool buffer, same low bits checked by caller
    for (; i < n; i++)
        dst[i] = src ? src[i] : 0;
}

static int ReadAt(HANDLE f, ULONG off, void *buf, ULONG n)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER o;

    o.QuadPart = off;
    if (!NT_SUCCESS(ZwReadFile(f, NULL, NULL, NULL, &io, buf, n, &o, NULL)) || io.Information != n)
        return -1;
    return 0;
}

// ---- outbound SMP2P (APPS -> ADSP), as Linux qcom_smp2p (DT smp2p-adsp: qcom,smem = <443>, <429>; mboxes = <&apcs_glb 10>) ----
// Item 429 is allocated in the APPS/ADSP SMEM partition like qcom_smem_alloc_private() under hwspinlock 3 (tcsr_mutex, stride 0x1000,
// APPS proc id 1); only shared RAM is written. Kick = APCS_GLB (0x0f111000) + 8, bit 10.
#define SMP2P_OUT_ITEM      429
#define SMP2P_IN_ITEM       443
#define TCSR_MUTEX3_PA      0x00343000u
#define APCS_GLB_PA         0x0f111000u
#define SMP2P_ITEM_SIZE     340                 // magic, version|features, pids, entries count, flags, 16 x (name[16], value)

static volatile UCHAR *g_out;
static int g_neg, g_ssr_en, g_ssr_ack;

static void AWr(volatile UCHAR *p, ULONG v)
{
    *(volatile ULONG *)p = v;
}

static void Smp2pKick(void)
{
    volatile UCHAR *apcs = Map(APCS_GLB_PA, 0x1000);

    if (!apcs)
        return;
    __asm__ volatile("dsb sy" ::: "memory");
    AWr(apcs + 8, 1u << g_rp->kick_bit);
    MmUnmapIoSpace((PVOID)apcs, 0x1000);
}

static volatile UCHAR *Smp2pFind(ULONG item)
{
    ULONG p, end, w0, sz, padh;

    if (!g_part)
        return NULL;
    end = ARd(g_part + 12);
    if (end > g_part_size)
        end = g_part_size;
    for (p = 32; p + 16 <= end; p += 16 + padh + sz) {
        volatile UCHAR *e = g_part + p;
        w0 = ARd(e);
        sz = ARd(e + 4);
        padh = ARd(e + 8) >> 16;
        if ((w0 & 0xffff) != 0xa5a5)
            return NULL;
        if ((w0 >> 16) == item)
            return (p + 16 + padh + SMP2P_ITEM_SIZE <= end) ? e + 16 + padh : NULL;
    }
    return NULL;
}

static int Smp2pOutCreate(void)
{
    volatile UCHAR *m, *it = NULL;
    ULONG p, end, cached, w0 = 0, sz, padh, i, owner = 0;
    int err = 0, created = 0;
    static const char name[16] = "master-kernel";

    if (!g_part)
        return -1;
    m = Map(TCSR_MUTEX3_PA, 0x1000);
    if (!m)
        return -1;
    for (i = 0; i < 100000; i++) {
        AWr(m, 1);
        if ((owner = ARd(m)) == 1)
            break;
    }
    if (owner != 1) {
        MmUnmapIoSpace((PVOID)m, 0x1000);
        Log("adsp: smem hwlock 3 busy (owner %lu)", (unsigned long)owner);
        return -1;
    }
    __asm__ volatile("msr daifset, #2" ::: "memory");   // as hwspin_lock_timeout_irqsave: short, no logging inside
    end = ARd(g_part + 12);
    cached = ARd(g_part + 16);
    if (end > g_part_size || cached > g_part_size || end > cached) {
        err = 1;
        goto unlock;
    }
    for (p = 32; p + 16 <= end; p += 16 + padh + sz) {
        w0 = ARd(g_part + p);
        sz = ARd(g_part + p + 4);
        padh = ARd(g_part + p + 8) >> 16;
        if ((w0 & 0xffff) != 0xa5a5) {
            err = 2;
            goto unlock;
        }
        if ((w0 >> 16) == g_rp->smp2p_out) {
            it = g_part + p + 16 + padh;
            goto unlock;
        }
    }
    if (p != end || end + 16 + 344 > cached) {
        err = 3;
        goto unlock;
    }
    {
        volatile UCHAR *e = g_part + end;
        WrBytes(e + 16, NULL, 344);
        AWr(e, 0xa5a5u | (g_rp->smp2p_out << 16));
        AWr(e + 4, 344);                                // ALIGN(340, 8)
        AWr(e + 8, 344 - SMP2P_ITEM_SIZE);              // padding_data 4, padding_hdr 0
        AWr(e + 12, 0);
        __asm__ volatile("dsb sy" ::: "memory");
        AWr(g_part + 12, end + 16 + 344);               // offset_free_uncached
        it = e + 16;
        created = 1;
    }
unlock:
    __asm__ volatile("dsb sy" ::: "memory");
    AWr(m, 0);
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    MmUnmapIoSpace((PVOID)m, 0x1000);
    if (err) {
        Log("adsp: smem alloc %lu failed (%d): free %08lx cached %08lx size %08lx at %08lx", (unsigned long)g_rp->smp2p_out, err, (unsigned long)end,
            (unsigned long)cached, (unsigned long)g_part_size, (unsigned long)p);
        return -1;
    }
    Log("adsp: smem item %lu %s at partition offset %06lx", (unsigned long)g_rp->smp2p_out, created ? "allocated" : "already there",
        (unsigned long)(it - g_part));
    if (created) {
        AWr(it, SMP2P_MAGIC);
        AWr(it + 4, 1u << 8);                           // version 0 until ready, features SSR_ACK
        AWr(it + 8, 0u | (g_rp->host << 16));            // local pid 0 (APPS), remote pid = host
        AWr(it + 12, 16u | (1u << 16));                 // total 16, valid 1
        AWr(it + 16, 0);                                // flags
        for (i = 0; i < 16; i += 4)
            AWr(it + 20 + i, *(const ULONG *)(name + i));
        AWr(it + 36, 0);                                // master-kernel = 0
        __asm__ volatile("dsb sy" ::: "memory");
        AWr(it + 4, 1u | (1u << 8));                    // version 1: item valid
    }
    g_out = it;
    g_neg = g_ssr_en = g_ssr_ack = 0;
    Smp2pKick();
    return 0;
}

// qcom_smp2p_negotiate + qcom_smp2p_do_ssr_ack, polled instead of the GIC SPI 279 interrupt
static void Smp2pService(void)
{
    volatile UCHAR *in = Smp2pFind(g_rp->smp2p_in);
    ULONG o;

    if (!g_out || !in || ARd(in) != SMP2P_MAGIC)
        return;
    if (!g_neg && (ARd(in + 4) & 0xff) == (ARd(g_out + 4) & 0xff)) {
        ULONG feat = (ARd(g_out + 4) >> 8) & (ARd(in + 4) >> 8);
        AWr(g_out + 4, (ARd(g_out + 4) & 0xff) | (feat << 8));
        g_neg = 1;
        g_ssr_en = feat & 1;
        Log("adsp: smp2p negotiated: in %08lx, features %lx", (unsigned long)ARd(in + 4), (unsigned long)feat);
    }
    if (g_ssr_en && (int)(ARd(in + 16) & 1) != g_ssr_ack) {
        g_ssr_ack = !g_ssr_ack;
        o = ARd(g_out + 16) & ~2u;
        if (g_ssr_ack)
            o |= 2;
        AWr(g_out + 16, o);
        Smp2pKick();
        Log("adsp: smp2p restart-done %d -> ack", g_ssr_ack);
    }
}

typedef struct { ULONG type, offset, vaddr, paddr, filesz, memsz, flags, align; } PHDR32;

static void RprocBoot(const RPROC *rp)
{
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES a;
    IO_STATUS_BLOCK io;
    HANDLE f = NULL;
    UCHAR *buf = NULL, *meta = NULL;
    volatile UCHAR *mem = NULL;
    PHDR32 ph[64];
    ULONG phoff, phnum, i, hash = 0, minpa = ~0u, maxpa = 0, metalen, v, w;
    PHYSICAL_ADDRESS lo, hi, bd, mp;
    long long r;
    U64 res;

    // run_54: 0x101 CLOCK_WATCHDOG_TIMEOUT inside PAS_INIT_IMAGE (CPU in TZ, no clock interrupts for 0x18 ticks) while this thread had
    // the timer at 1 ms (HwThread: ExSetTimerResolution 10000) -> back to the default tick for the TZ calls, restored at the end
    {
        ULONG cur = ExSetTimerResolution(0, FALSE);
        g_rp = rp;
        Log("=== adsp: start %s (PAS id %d, carveout %08lx+%08lx), timer resolution now %lu x100ns", rp->name, rp->pas_id,
            (unsigned long)rp->mem_pa, (unsigned long)rp->mem_size,
            (unsigned long)cur);
        if (cur < 100000) {                             // < 10 ms: someone else keeps the timer fast -> watchdog window too short
            Log("adsp: timer still faster than 10 ms -> skip (no TZ calls)");
            ExSetTimerResolution(10000, TRUE);
            return;
        }
    }
    SleepMs(300);
    if (SmemOpenPart() == 0) {
        Log("adsp: smp2p before: %08lx", (unsigned long)Smp2pRead(1));
        if (Smp2pOutCreate())
            Log("adsp: no outbound smp2p (ADSP will not report ready)");
    }
    SleepMs(300);
    buf = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, CHUNK, ADSP_TAG);
    if (!buf)
        goto out;
    RtlInitUnicodeString(&u, rp->fw);
    InitializeObjectAttributes(&a, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&f, FILE_READ_DATA | SYNCHRONIZE, &a, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        Log("adsp: no firmware file for %s", rp->name);
        f = NULL;
        goto out;
    }
    if (ReadAt(f, 0, buf, 52) || *(ULONG *)buf != 0x464c457f || buf[4] != 1) {
        Log("adsp: not ELF32");
        goto out;
    }
    phoff = *(ULONG *)(buf + 28);
    phnum = *(USHORT *)(buf + 44);
    if (*(USHORT *)(buf + 42) != 32 || phnum < 2 || phnum > 64 || ReadAt(f, phoff, ph, phnum * 32)) {
        Log("adsp: bad phdrs (%lu)", (unsigned long)phnum);
        goto out;
    }
    for (i = 1; i < phnum; i++)
        if (((ph[i].flags >> 24) & 7) == 2) { hash = i; break; }
    for (i = 0; i < phnum; i++) {
        if (ph[i].type != 1 || ((ph[i].flags >> 24) & 7) == 2 || !ph[i].memsz)
            continue;
        if (ph[i].filesz > ph[i].memsz)
            goto out;
        if (ph[i].paddr < minpa) minpa = ph[i].paddr;
        if (ph[i].paddr + ph[i].memsz > maxpa) maxpa = (ph[i].paddr + ph[i].memsz + 0xfff) & ~0xfffu;
    }
    Log("adsp: %lu phdrs, hash seg %lu (%lu bytes), header %lu bytes, span %08lx..%08lx", (unsigned long)phnum, (unsigned long)hash,
        hash ? (unsigned long)ph[hash].filesz : 0, (unsigned long)ph[0].filesz, (unsigned long)minpa, (unsigned long)maxpa);
    if (!hash || ph[0].type == 1 || maxpa - minpa > rp->mem_size || ph[0].filesz > 0x1000 || ph[hash].filesz > 0x8000) {
        Log("adsp: layout not as expected -> stop");
        goto out;
    }

    // metadata, physically contiguous, below 4 GB, uncached
    metalen = ph[0].filesz + ph[hash].filesz;
    lo.QuadPart = 0; hi.QuadPart = 0xffffffffLL; bd.QuadPart = 0;
    meta = (UCHAR *)MmAllocateContiguousMemorySpecifyCache((metalen + 0xfff) & ~0xfffu, lo, hi, bd, MmNonCached);
    if (!meta || ReadAt(f, 0, buf, ph[0].filesz))
        goto out;
    WrBytes(meta, buf, ph[0].filesz);
    if (ReadAt(f, ph[hash].offset, buf, ph[hash].filesz))
        goto out;
    WrBytes(meta + ph[0].filesz, buf, ph[hash].filesz);
    __asm__ volatile("dsb sy" ::: "memory");
    mp = MmGetPhysicalAddress(meta);
    Log("adsp: metadata %lu bytes at pa %llx", (unsigned long)metalen, (unsigned long long)mp.QuadPart);
    SleepMs(300);

    if (CryptoVotes()) {
        Log("adsp: crypto bus votes failed -> stop (no TZ calls)");
        goto out;
    }
    SleepMs(300);
    if (RpmLevel(rp->votes[0], rp->vote_names[0]) || (rp->votes[1] && RpmLevel(rp->votes[1], rp->vote_names[1]))) {
        Log("adsp: rpm votes failed -> stop");
        goto out;
    }

    r = Pil(1, 0x82, rp->pas_id, (U64)mp.QuadPart, 0, &res);       // QCOM_SCM_ARGS(2, VAL, RW)
    Log("adsp: init_image -> x0 %lld res %llu", r, res);
    SleepMs(300);
    if (r || res)
        goto out;
    r = Pil(2, 3, rp->pas_id, rp->mem_pa, maxpa - minpa, &res);
    Log("adsp: mem_setup(%08lx, %08lx) -> x0 %lld res %llu", rp->mem_pa, (unsigned long)(maxpa - minpa), r, res);
    SleepMs(300);
    if (r || res)
        goto shutdown;

    mem = Map(rp->mem_pa, rp->mem_size);          // device memory; WrBytes uses aligned 32-bit stores only
    if (!mem)
        goto shutdown;
    for (i = 0; i < phnum; i++) {
        ULONG done = 0, dst;
        if (ph[i].type != 1 || ((ph[i].flags >> 24) & 7) == 2 || !ph[i].memsz)
            continue;
        dst = ph[i].paddr - minpa;
        if ((ph[i].offset & 3) != (dst & 3)) {
            Log("adsp: seg %lu misaligned -> stop", (unsigned long)i);
            goto shutdown;
        }
        while (done < ph[i].filesz) {
            ULONG skew = (ph[i].offset + done) & 3, n = ph[i].filesz - done;
            if (n > CHUNK - 4)
                n = CHUNK - 4;
            if (ReadAt(f, ph[i].offset + done, buf + skew, n)) {
                Log("adsp: read seg %lu failed", (unsigned long)i);
                goto shutdown;
            }
            WrBytes(mem + dst + done, buf + skew, n);
            done += n;
        }
        if (ph[i].memsz > ph[i].filesz)
            WrBytes(mem + dst + ph[i].filesz, NULL, ph[i].memsz - ph[i].filesz);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    Log("adsp: segments loaded");
    SleepMs(300);

    r = Pil(5, 1, rp->pas_id, 0, 0, &res);
    Log("adsp: auth_and_reset -> x0 %lld res %llu", r, res);
    SleepMs(300);
    if (r || res)
        goto shutdown;

    v = ~0u;
    for (w = 0; w < 100 && g_part; w++) {
        ULONG nv;
        Smp2pService();
        nv = Smp2pRead(0);
        if (nv != v) {
            Log("adsp: t+%lu ms smp2p value %08lx%s%s", (unsigned long)w * 100, (unsigned long)nv,
                (nv != ~0u && (nv & 2)) ? " READY" : "", (nv != ~0u && (nv & 4)) ? " HANDOVER" : "");
            v = nv;
        }
        if (v != ~0u && (v & 2))
            break;
        SleepMs(100);
    }
    if (g_part) {
        Log("adsp: smp2p after: %08lx", (unsigned long)Smp2pRead(1));
        if (g_out)
            Log("adsp: smp2p out: %08lx %08lx %08lx %08lx", (unsigned long)ARd(g_out + 4), (unsigned long)ARd(g_out + 8),
                (unsigned long)ARd(g_out + 12), (unsigned long)ARd(g_out + 16));
    }
    Log("adsp: %s (votes kept)", (v != ~0u && (v & 2)) ? "ADSP UP" : "no ready bit in 10 s (ADSP left running as is)");
    goto out;

shutdown:
    r = Pil(6, 1, rp->pas_id, 0, 0, &res);
    Log("adsp: shutdown -> x0 %lld res %llu", r, res);
out:
    if (f) ZwClose(f);
    if (mem) MmUnmapIoSpace((PVOID)mem, rp->mem_size);
    if (g_part) { MmUnmapIoSpace((PVOID)g_part, g_part_size); g_part = NULL; g_out = NULL; }
    if (meta) MmFreeContiguousMemory(meta);
    if (buf) ExFreePoolWithTag(buf, ADSP_TAG);
    ExSetTimerResolution(10000, TRUE);
    Log("=== adsp: end");
}

// delete-on-close open: 1 if the arm file existed (and is now gone)
static int RprocArmed(const RPROC *rp)
{
    return FileOp(rp->arm, 0);
}
