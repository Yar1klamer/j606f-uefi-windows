// J606F DSI probe (Windows ARM64 kernel driver, test-signed, legacy: sc start / DriverEntry does the work and returns).
// Step 1 of the Windows display driver (owner's choice A, 2026-11-07): can Windows send the DCS brightness command 0x51 to the
// Himax HX83102P panel while the video stream set up by the boot loader keeps running? (pmOS: brightness = DCS 0x51, 12 bit, big endian.)
// 1. Read only: DSI0 controller registers (0x05e94000, DSI 6G: registers at +4, as dsi_host.c io_offset), apps SMMU (0x0c600000) stream
//    matches for the MDSS stream IDs 0x420/0x421 and their S2CR / context bank (bypass or translation?).
// 2. Writes (DSI only): DCS long write 39 [51 hi lo] through the command DMA engine, as dsi_host.c does it in video mode:
//    CTRL |= CMD_MODE_EN | ENABLE, TRIG_CTRL DMA trigger = SW (+ BLOCK_DMA_WITHIN_FRAME on 6G >= 1.2), wait VIDEO_DONE, DMA_BASE/LEN, TRIG_DMA,
//    poll CMD_DMA_DONE, restore CTRL. The command buffer is the last page of the boot loader's display region (0x5cfff000, reserved
//    in the UEFI memory map). Values: 4095, 600, 100, 2000 (6 s each; the owner watches the screen). Log: C:\j606f\out\dsiprobe.txt.
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
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\dsiprobe.txt");
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &attrs, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                                FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(file, NULL, NULL, NULL, &iosb, (PVOID)text, len, NULL, NULL);
        ZwClose(file);
    }
}

static void Log(const char *fmt, ...)
{
    char line[256];
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
    DbgPrint("j606f dsiprobe: %s", line);
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

static void SleepMs(LONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(0 /* KernelMode */, FALSE, &t);
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

#define DSI0_PA      0x05e94000
#define DSI_SHIFT    4
#define R_CTRL       (DSI_SHIFT + 0x000)
#define R_STATUS0    (DSI_SHIFT + 0x004)
#define R_CMD_DMA    (DSI_SHIFT + 0x038)
#define R_DMA_BASE   (DSI_SHIFT + 0x044)
#define R_DMA_LEN    (DSI_SHIFT + 0x048)
#define R_ACK_ERR    (DSI_SHIFT + 0x064)
#define R_TRIG_CTRL  (DSI_SHIFT + 0x080)
#define R_TRIG_DMA   (DSI_SHIFT + 0x08c)
#define R_INTR       (DSI_SHIFT + 0x10c)
#define I_DMA_DONE   (1u << 0)
#define I_MDP_DONE   (1u << 8)
#define I_VID_DONE   (1u << 16)
#define I_BTA_DONE   (1u << 20)
#define I_ERROR      (1u << 24)
#define I_STATUS     (I_DMA_DONE | I_MDP_DONE | I_VID_DONE | I_BTA_DONE | I_ERROR)
#define SMMU_PA      0x0c600000
#define BUF_PA       0x5cfff000

static volatile UCHAR *g_dsi;
static volatile UCHAR *g_buf;

static void DumpDsi(const char *tag)
{
    char line[200];
    ULONG off, i;
    int p;

    Log("--- DSI0 registers (%s), offsets from 0x05e94000", tag);
    for (off = 0; off < 0x300; off += 0x20) {
        p = Fmt(line, sizeof(line), "%03lx:", (unsigned long)off);
        for (i = 0; i < 0x20; i += 4)
            p += Fmt(line + p, sizeof(line) - p, " %08lx", (unsigned long)Rd(g_dsi, off + i));
        Log("%s", line);
    }
}

static void DumpSmmu(void)
{
    volatile UCHAR *s = Map(SMMU_PA, 0x1000);
    ULONG cr0, idr0, idr1, n, i, smr, s2cr, found = 0;

    if (!s)
        return;
    cr0 = Rd(s, 0x0);
    idr0 = Rd(s, 0x20);
    idr1 = Rd(s, 0x24);
    n = idr0 & 0xff;
    Log("SMMU sCR0 %08lx (CLIENTPD %lu, USFCFG %lu) IDR0 %08lx IDR1 %08lx, %lu stream match groups", (unsigned long)cr0,
        (unsigned long)(cr0 & 1), (unsigned long)((cr0 >> 10) & 1), (unsigned long)idr0, (unsigned long)idr1, (unsigned long)n);
    if (n > 128)
        n = 128;
    for (i = 0; i < n; i++) {
        smr = Rd(s, 0x800 + 4 * i);
        s2cr = Rd(s, 0xc00 + 4 * i);
        if (!(smr & 0x80000000u))
            continue;
        found++;
        if (((smr & 0x7fff) & ~((smr >> 16) & 0x7fff)) == (0x420 & ~((smr >> 16) & 0x7fff)) ||
            ((smr & 0x7fff) & ~((smr >> 16) & 0x7fff)) == (0x421 & ~((smr >> 16) & 0x7fff)) || found <= 40)
            Log("SMR%lu %08lx (id %03lx mask %03lx) S2CR %08lx (type %lu: 0 trans 1 bypass 2 fault, cb %lu)", (unsigned long)i,
                (unsigned long)smr, (unsigned long)(smr & 0x7fff), (unsigned long)((smr >> 16) & 0x7fff), (unsigned long)s2cr,
                (unsigned long)((s2cr >> 16) & 3), (unsigned long)(s2cr & 0xff));
    }
    Log("SMMU: %lu valid SMRs", (unsigned long)found);
    MmUnmapIoSpace((PVOID)s, 0x1000);
}

static int WaitBit(ULONG reg, ULONG bit, ULONG us)
{
    ULONG t;
    for (t = 0; t < us; t += 20) {
        if (Rd(g_dsi, reg) & bit)
            return 0;
        KeStallExecutionProcessor(20);
    }
    return -1;
}

static void SendBrightness(ULONG ver, ULONG level)
{
    ULONG ctrl = Rd(g_dsi, R_CTRL), trig = Rd(g_dsi, R_TRIG_CTRL), intr = Rd(g_dsi, R_INTR), st0, ntrig;
    int vid, done;

    g_buf[0] = 3;                   // word count lo
    g_buf[1] = 0;                   // word count hi
    g_buf[2] = 0x39;                // DCS long write, VC 0
    g_buf[3] = 0x80 | 0x40;         // last packet, long
    g_buf[4] = 0x51;
    g_buf[5] = (UCHAR)(level >> 8);
    g_buf[6] = (UCHAR)level;
    g_buf[7] = 0xff;
    __asm__ volatile("dsb sy" ::: "memory");
    ntrig = (trig & ~7u) | 4u;      // DMA trigger = SW
    if (ver >= 0x10020000)
        ntrig |= 1u << 12;          // BLOCK_DMA_WITHIN_FRAME
    Wr(g_dsi, R_TRIG_CTRL, ntrig);
    Wr(g_dsi, R_CTRL, ctrl | 4u | 1u);
    Wr(g_dsi, R_INTR, (intr & ~I_STATUS) | I_STATUS);   // clear status bits (write 1), keep masks
    st0 = Rd(g_dsi, R_STATUS0);
    vid = -2;
    if (st0 & (1u << 3)) {          // video engine busy: send after the frame, as dsi_wait4video_eng_busy()
        vid = WaitBit(R_INTR, I_VID_DONE, 50000);
        KeStallExecutionProcessor(2000);
    }
    Wr(g_dsi, R_DMA_BASE, BUF_PA);
    Wr(g_dsi, R_DMA_LEN, 8);
    Wr(g_dsi, R_TRIG_DMA, 1);
    done = WaitBit(R_INTR, I_DMA_DONE, 100000);
    Log("level %4lu: ctrl %08lx trig %08lx->%08lx intr %08lx status0 %08lx video_done %d dma_done %d | after: intr %08lx status0 %08lx ack_err %08lx",
        (unsigned long)level, (unsigned long)ctrl, (unsigned long)trig, (unsigned long)ntrig, (unsigned long)intr, (unsigned long)st0, vid,
        done, (unsigned long)Rd(g_dsi, R_INTR), (unsigned long)Rd(g_dsi, R_STATUS0), (unsigned long)Rd(g_dsi, R_ACK_ERR));
    Wr(g_dsi, R_INTR, (Rd(g_dsi, R_INTR) & ~I_STATUS) | I_DMA_DONE | I_VID_DONE);
    Wr(g_dsi, R_CTRL, ctrl);
}

static VOID ProbeUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    static const ULONG levels[] = { 4095, 600, 100, 2000 };
    ULONG ver, i;

    UNREFERENCED_PARAMETER(reg);
    drv->DriverUnload = ProbeUnload;
    Log("=== j606f dsiprobe start");
    g_dsi = Map(DSI0_PA, 0x1000);
    g_buf = Map(BUF_PA, 0x1000);
    if (!g_dsi || !g_buf)
        return STATUS_SUCCESS;
    ver = Rd(g_dsi, 0);
    Log("DSI 6G HW_VERSION %08lx (major %lu minor %lu step %lu)", (unsigned long)ver, (unsigned long)(ver >> 28),
        (unsigned long)((ver >> 16) & 0xfff), (unsigned long)(ver & 0xffff));
    DumpDsi("before");
    DumpSmmu();
    if (!(Rd(g_dsi, R_CTRL) & 1)) {
        Log("DSI0 not enabled: no commands sent");
    } else {
        for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
            SendBrightness(ver, levels[i]);
            SleepMs(6000);
        }
        DumpDsi("after");
    }
    MmUnmapIoSpace((PVOID)g_buf, 0x1000);
    MmUnmapIoSpace((PVOID)g_dsi, 0x1000);
    Log("=== j606f dsiprobe end");
    return STATUS_SUCCESS;
}
