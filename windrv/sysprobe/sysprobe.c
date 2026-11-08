// J606F system probe (Windows ARM64 kernel driver, test-signed, legacy service). READ ONLY: nothing is written to any device register
// (SPMI reads use the observer channel command register, as hidbuttons.c). Purpose: inputs for CPU frequency, Bluetooth, Wi-Fi, sleep work.
//  1. cpufreq-hw (0xf521000 domain0 cpu0-3, 0xf523000 domain1 cpu4-7): enable, dcvs_ctrl, current vote, perf state, LUT (freq = 19.2 MHz * L,
//     src 0 = 300 MHz) and voltages (qcom-cpufreq-hw.c, qcom_soc_data).
//  2. tsens v2 (TM 0x4411000, SROT 0x4410000): ctrl, Sn_STATUS last temp (0.1 C, 12-bit signed), valid bit.
//  3. GCC (always clocked): APCS vote 0x7900c, CBCRs QUP0 S3 (BT UART), QUP0 m/s AHB, AHB2PHY_USB, QUSB2PHY BCR, USB30 BCR.
//  4. QUP0 SE3 (0x4a8c000, BT UART in Linux): only if the S3 CBCR says the clock is on — GENI FW revision, IF_DISABLE, status.
//  5. RPM message RAM (0x45f0000, 0x7000): GLINK TOC (magic "grt0", entries ap2r/r2ap) + FIFO head/tail words.
//  6. PM6125 (SPMI SID 0) LDO1..LDO24 (base 0x4000 + 0x100*(n-1)): TYPE/SUBTYPE 0x04/0x05, STATUS 0x08, VSET 0x40/0x41, MODE 0x45, EN 0x46.
//     BT/Wi-Fi in Linux: L9, L16, L17, L23 (pmOS notes wifi_bt.md); camera CSIPHY L18.
// Log: C:\j606f\out\sysprobe.txt.
#include "j606f_wdm.h"

static void LogRaw(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\sysprobe.txt");
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

static int Fmt(char *buf, ULONG size, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n = 0;

    __builtin_va_start(ap, fmt);
    _vsnprintf(buf, size - 1, fmt, ap);
    __builtin_va_end(ap);
    buf[size - 1] = 0;
    while (buf[n])
        n++;
    return n;
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

static void Unmap(volatile UCHAR *va, SIZE_T size)
{
    if (va)
        MmUnmapIoSpace((PVOID)va, size);
}

// ---------------------------------------------------------------------------------------------------------------------------------
static void CpufreqDump(int dom, ULONG pa)
{
    volatile UCHAR *b = Map(pa, 0x1000);
    ULONG i, prev = 0xffffffff;

    if (!b)
        return;
    Log("cpufreq dom%d %08lx: enable %08lx dcvs_ctrl %08lx current_vote %08lx perf_state %08lx", dom, (unsigned long)pa,
        (unsigned long)Rd(b, 0x0), (unsigned long)Rd(b, 0xbc), (unsigned long)Rd(b, 0x704), (unsigned long)Rd(b, 0x920));
    for (i = 0; i < 40; i++) {
        ULONG f = Rd(b, 0x110 + 32 * i), v = Rd(b, 0x114 + 32 * i);
        ULONG src = (f >> 30) & 3, l = f & 0xff, cores = (f >> 16) & 7;
        ULONG mhz = src ? (192 * l) / 10 : 300;
        Log("  lut%02lu freq %08lx (src %lu L %lu cores %lu = %lu MHz) volt %08lx (%lu mV)", (unsigned long)i, (unsigned long)f,
            (unsigned long)src, (unsigned long)l, (unsigned long)cores, (unsigned long)mhz, (unsigned long)v, (unsigned long)(v & 0xfff));
        if (i && f == prev)
            break;
        prev = f;
    }
    {
        char line[260];
        int n = 0;
        for (i = 0; i < 0x100; i += 4)
            n += Fmt(line + n, sizeof(line) - n, " %02lx", (unsigned long)(Rd(b, i) & 0xff));
        Log("  regs 0..ff (low bytes):%s", line);
    }
    Unmap(b, 0x1000);
}

static void TsensDump(void)
{
    volatile UCHAR *tm = Map(0x04411000, 0x1000), *srot = Map(0x04410000, 0x1000);
    char line[280];
    int n = 0, i;

    if (srot)
        Log("tsens srot hw_ver %08lx ctrl %08lx period %08lx", (unsigned long)Rd(srot, 0), (unsigned long)Rd(srot, 4), (unsigned long)Rd(srot, 8));
    if (tm) {
        Log("tsens tm int_en %08lx trdy %08lx", (unsigned long)Rd(tm, 4), (unsigned long)Rd(tm, 0xe4));
        for (i = 0; i < 16; i++) {
            ULONG s = Rd(tm, 0xa0 + 4 * i);
            LONG t = (LONG)(s & 0xfff);
            if (t & 0x800)
                t -= 0x1000;
            n += Fmt(line + n, sizeof(line) - n, " s%d=%s%ld.%ld", i, (s >> 21) & 1 ? "" : "?", (long)(t / 10), (long)((t < 0 ? -t : t) % 10));
        }
        Log("tsens temps C:%s", line);
    }
    Unmap(tm, 0x1000);
    Unmap(srot, 0x1000);
}

static void GccDump(void)
{
    volatile UCHAR *g = Map(0x01400000, 0x80000);

    if (!g)
        return;
    Log("gcc apcs_vote 7900c %08lx", (unsigned long)Rd(g, 0x7900c));
    Log("gcc qup0 m_ahb 1f004 %08lx s_ahb 1f008 %08lx s0 1f018? s3 cbcr 1f4d4 %08lx", (unsigned long)Rd(g, 0x1f004), (unsigned long)Rd(g, 0x1f008),
        (unsigned long)Rd(g, 0x1f4d4));
    Log("gcc ahb2phy_usb 1d008 %08lx qusb2phy_bcr 1c000 %08lx usb30_bcr 1a000 %08lx", (unsigned long)Rd(g, 0x1d008), (unsigned long)Rd(g, 0x1c000),
        (unsigned long)Rd(g, 0x1a000));
    Log("gcc qup0 s3 rcgr 1f4e4? cmd %08lx cfg %08lx", (unsigned long)Rd(g, 0x1f4e4), (unsigned long)Rd(g, 0x1f4e8));
    Unmap(g, 0x80000);
}

static void Se3Dump(void)
{
    volatile UCHAR *g = Map(0x01400000, 0x80000), *se;
    ULONG cbcr, m, s;

    if (!g)
        return;
    cbcr = Rd(g, 0x1f4d4);
    m = Rd(g, 0x1f004);
    s = Rd(g, 0x1f008);
    Unmap(g, 0x80000);
    if ((cbcr & 0x80000000) || (m & 0x80000000) || (s & 0x80000000)) {
        Log("se3: clocks off (s3 %08lx m %08lx s %08lx) - registers not read", (unsigned long)cbcr, (unsigned long)m, (unsigned long)s);
        return;
    }
    se = Map(0x04a8c000, 0x4000);
    if (!se)
        return;
    Log("se3: geni fw_rev_ro 0x68 %08lx s_fw 0x6c %08lx if_disable_ro 0x64 %08lx status 0x40 %08lx ser_m_clk 0x48 %08lx dma_mode 0x258 %08lx",
        (unsigned long)Rd(se, 0x68), (unsigned long)Rd(se, 0x6c), (unsigned long)Rd(se, 0x64), (unsigned long)Rd(se, 0x40),
        (unsigned long)Rd(se, 0x48), (unsigned long)Rd(se, 0x258));
    Unmap(se, 0x4000);
}

static void RpmDump(void)
{
    volatile UCHAR *r = Map(0x045f0000, 0x7000);
    ULONG magic, count, i;

    if (!r)
        return;
    magic = Rd(r, 0x7000 - 256);
    count = Rd(r, 0x7000 - 256 + 4);
    Log("rpm msg ram toc magic %08lx (grt0 = 67727430) count %lu", (unsigned long)magic, (unsigned long)count);
    if (magic == 0x67727430 && count <= 20)
        for (i = 0; i < count; i++) {
            ULONG e = 0x7000 - 256 + 8 + 12 * i, id = Rd(r, e), off = Rd(r, e + 4), sz = Rd(r, e + 8);
            Log("  toc[%lu] id %08lx off %05lx size %lx  head/tail words %08lx %08lx", (unsigned long)i, (unsigned long)id, (unsigned long)off,
                (unsigned long)sz, off + 8 < 0x7000 ? (unsigned long)Rd(r, off) : 0UL, off + 8 < 0x7000 ? (unsigned long)Rd(r, off + 4) : 0UL);
        }
    {
        char line[280];
        int n = 0;
        for (i = 0; i < 0x40; i += 4)
            n += Fmt(line + n, sizeof(line) - n, " %08lx", (unsigned long)Rd(r, i));
        Log("rpm msg ram 0..3f:%s", line);
    }
    Unmap(r, 0x7000);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// SPMI read only
#define SPMI_CORE_PA 0x01c40000
#define SPMI_OBS_PA  0x03e00000
static volatile UCHAR *g_core;

static volatile UCHAR *SpmiChannel(USHORT sid, USHORT addr)
{
    USHORT ppid = (USHORT)((sid << 8) | (addr >> 8));
    ULONG n;

    for (n = 0; 0x900 + 4 * n < 0x1100; n++)
        if (((Rd(g_core, 0x900 + 4 * n) >> 8) & 0xfff) == ppid)
            return Map(SPMI_OBS_PA + 0x80 * n, 0x80);
    return NULL;
}

static int SpmiRead(volatile UCHAR *ch, USHORT addr)
{
    ULONG st = 0;
    int i;

    Wr(ch, 0x00, (1u << 27) | ((addr & 0xffu) << 4));
    for (i = 0; i < 2000; i++) {
        st = Rd(ch, 0x08);
        if (st & 1)
            break;
    }
    if ((st & 0xf) != 1)
        return -1;
    return (int)(Rd(ch, 0x18) & 0xff);
}

static void LdoDump(void)
{
    ULONG ver;
    int n;

    g_core = Map(SPMI_CORE_PA, 0x2000);
    if (!g_core)
        return;
    ver = Rd(g_core, 0);
    Log("SPMI arbiter %08lx", (unsigned long)ver);
    if (ver < 0x50000000 || ver >= 0x70000000)
        goto out;
    for (n = 1; n <= 24; n++) {
        USHORT base = (USHORT)(0x4000 + 0x100 * (n - 1));
        volatile UCHAR *ch = SpmiChannel(0, base);
        int ty, st, lo, hi, mode, en;
        if (!ch) {
            Log("pm6125 L%d %04x: no channel", n, base);
            continue;
        }
        ty = SpmiRead(ch, base + 0x04) | (SpmiRead(ch, base + 0x05) << 8);
        st = SpmiRead(ch, base + 0x08);
        lo = SpmiRead(ch, base + 0x40);
        hi = SpmiRead(ch, base + 0x41);
        mode = SpmiRead(ch, base + 0x45);
        en = SpmiRead(ch, base + 0x46);
        Log("pm6125 L%-2d %04x type %04x status %02x vset %02x%02x (%d mV) mode %02x en %02x", n, base, ty & 0xffff, st & 0xff, hi & 0xff, lo & 0xff,
            ((hi & 0xff) << 8) | (lo & 0xff), mode & 0xff, en & 0xff);
        Unmap(ch, 0x80);
    }
    {
        // S-bucks (FTS/HF) 0x1400.. for reference: SMPS1..8 at 0x1400 + 0x300*(n-1)
        for (n = 1; n <= 8; n++) {
            USHORT base = (USHORT)(0x1400 + 0x300 * (n - 1));
            volatile UCHAR *ch = SpmiChannel(0, base);
            if (!ch)
                continue;
            Log("pm6125 S%d %04x type %02x%02x status %02x vset %02x%02x en %02x", n, base, SpmiRead(ch, base + 5) & 0xff, SpmiRead(ch, base + 4) & 0xff,
                SpmiRead(ch, base + 8) & 0xff, SpmiRead(ch, base + 0x41) & 0xff, SpmiRead(ch, base + 0x40) & 0xff, SpmiRead(ch, base + 0x46) & 0xff);
            Unmap(ch, 0x80);
        }
    }
out:
    Unmap(g_core, 0x2000);
}

// ---------------------------------------------------------------------------------------------------------------------------------
static VOID Unload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = Unload;
    Log("=== j606f sysprobe start (read only)");
    GccDump();
    CpufreqDump(0, 0x0f521000);
    CpufreqDump(1, 0x0f523000);
    TsensDump();
    Se3Dump();
    RpmDump();
    LdoDump();
    TsensDump();
    Log("=== j606f sysprobe end");
    return STATUS_SUCCESS;
}
