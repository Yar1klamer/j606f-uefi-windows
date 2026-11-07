// J606F additions to the Microsoft KMDOD sample (video/KMDOD, MS-PL; sources only on the build host, not published):
// brightness of the Himax HX83102P panel through DSI0 DCS 0x51 (12 bit), exposed to Windows as DXGK_BRIGHTNESS_INTERFACE v1
// (GUID_DEVINTERFACE_BRIGHTNESS, levels 0..100 %), plus a small file log C:\j606f\out\j606fdsp.txt.
// DSI command path = windrv/dsiprobe (tested run_22, 2026-11-07): CMD_MODE_EN, TRIG_CTRL SW + BLOCK_DMA_WITHIN_FRAME, wait VIDEO_DONE,
// DMA from 0x5cfff000 (last page of the boot loader display region, SMMU CB3 maps it 1:1), poll CMD_DMA_DONE, restore CTRL.
#include "BDD.hxx"

#define DSI0_PA      0x05e94000
#define R_CTRL       (4 + 0x000)
#define R_STATUS0    (4 + 0x004)
#define R_DMA_BASE   (4 + 0x044)
#define R_DMA_LEN    (4 + 0x048)
#define R_TRIG_CTRL  (4 + 0x080)
#define R_TRIG_DMA   (4 + 0x08c)
#define R_INTR       (4 + 0x10c)
#define I_DMA_DONE   (1u << 0)
#define I_VID_DONE   (1u << 16)
#define I_STATUS     ((1u << 0) | (1u << 8) | (1u << 16) | (1u << 20) | (1u << 24))
#define BUF_PA       0x5cfff000

static volatile UCHAR *g_dsi;
static volatile UCHAR *g_buf;
static UCHAR g_level = 100;
static KMUTEX g_blLock;
static LONG g_logLines;

extern "C" void J606fLog(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    size_t n;
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE f;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || InterlockedIncrement(&g_logLines) > 400)
        return;
    va_start(ap, fmt);
    RtlStringCbVPrintfA(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    RtlStringCbLengthA(line, sizeof(line), &n);
    line[n] = '\r';
    line[n + 1] = '\n';
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\j606fdsp.txt");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&f, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0))) {
        ZwWriteFile(f, NULL, NULL, NULL, &iosb, line, (ULONG)(n + 2), NULL, NULL);
        ZwClose(f);
    }
}

static ULONG Rd(ULONG off) { ULONG v = *(volatile ULONG *)(g_dsi + off); KeMemoryBarrier(); return v; }
static void Wr(ULONG off, ULONG v) { KeMemoryBarrier(); *(volatile ULONG *)(g_dsi + off) = v; KeMemoryBarrier(); }

static int WaitBit(ULONG reg, ULONG bit, ULONG us)
{
    for (ULONG t = 0; t < us; t += 20) {
        if (Rd(reg) & bit)
            return 0;
        KeStallExecutionProcessor(20);
    }
    return -1;
}

static int MapDsi(void)
{
    PHYSICAL_ADDRESS p;
    if (!g_dsi) {
        p.QuadPart = DSI0_PA;
        g_dsi = (volatile UCHAR *)MmMapIoSpaceEx(p, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (!g_buf) {
        p.QuadPart = BUF_PA;
        g_buf = (volatile UCHAR *)MmMapIoSpaceEx(p, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    }
    return g_dsi && g_buf ? 0 : -1;
}

// 12-bit panel level for 0..100 %: 0 % stays faintly lit (owner: level 100 was "very dim" but visible)
static ULONG PanelLevel(UCHAR pct)
{
    if (pct > 100)
        pct = 100;
    return 40 + (ULONG)pct * (4095 - 40) / 100;
}

static NTSTATUS SendLevel(ULONG level)
{
    ULONG ctrl, trig, intr, ver;
    int vid, done;

    if (MapDsi())
        return STATUS_INSUFFICIENT_RESOURCES;
    ctrl = Rd(R_CTRL);
    if (!(ctrl & 1))
        return STATUS_DEVICE_NOT_READY;
    ver = Rd(0);
    trig = Rd(R_TRIG_CTRL);
    intr = Rd(R_INTR);
    g_buf[0] = 3; g_buf[1] = 0; g_buf[2] = 0x39; g_buf[3] = 0xC0;
    g_buf[4] = 0x51; g_buf[5] = (UCHAR)(level >> 8); g_buf[6] = (UCHAR)level; g_buf[7] = 0xff;
    KeMemoryBarrier();
    Wr(R_TRIG_CTRL, (trig & ~7u) | 4u | (ver >= 0x10020000 ? (1u << 12) : 0));
    Wr(R_CTRL, ctrl | 4u | 1u);
    Wr(R_INTR, (intr & ~I_STATUS) | I_STATUS);
    vid = -2;
    if (Rd(R_STATUS0) & (1u << 3)) {
        vid = WaitBit(R_INTR, I_VID_DONE, 50000);
        KeStallExecutionProcessor(2000);
    }
    Wr(R_DMA_BASE, BUF_PA);
    Wr(R_DMA_LEN, 8);
    Wr(R_TRIG_DMA, 1);
    done = WaitBit(R_INTR, I_DMA_DONE, 100000);
    Wr(R_INTR, (Rd(R_INTR) & ~I_STATUS) | I_DMA_DONE | I_VID_DONE);
    Wr(R_CTRL, ctrl);
    J606fLog("brightness: panel level %lu, video_done %d dma_done %d", level, vid, done);
    return done == 0 ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static VOID BlRef(PVOID ctx) { UNREFERENCED_PARAMETER(ctx); }
static VOID BlDeref(PVOID ctx) { UNREFERENCED_PARAMETER(ctx); }

static NTSTATUS BlGetPossible(PVOID ctx, ULONG size, PUCHAR count, PUCHAR levels)
{
    UNREFERENCED_PARAMETER(ctx);
    ULONG n = 0;
    for (ULONG p = 0; p <= 100 && n < size; p += 5)
        levels[n++] = (UCHAR)p;
    *count = (UCHAR)n;
    J606fLog("brightness: GetPossible size %lu -> %lu levels", size, n);
    return STATUS_SUCCESS;
}

static NTSTATUS BlSet(PVOID ctx, UCHAR pct)
{
    UNREFERENCED_PARAMETER(ctx);
    NTSTATUS st;
    KeWaitForSingleObject(&g_blLock, Executive, KernelMode, FALSE, NULL);
    J606fLog("brightness: Set %u %%", pct);
    st = SendLevel(PanelLevel(pct));
    if (NT_SUCCESS(st))
        g_level = pct > 100 ? 100 : pct;
    KeReleaseMutex(&g_blLock, FALSE);
    return st;
}

static NTSTATUS BlGet(PVOID ctx, PUCHAR pct)
{
    UNREFERENCED_PARAMETER(ctx);
    *pct = g_level;
    return STATUS_SUCCESS;
}

extern "C" void J606fInit(void)
{
    KeInitializeMutex(&g_blLock, 0);
}

extern "C" NTSTATUS APIENTRY J606fQueryInterface(_In_ CONST PVOID ctx, _In_ PQUERY_INTERFACE qi)
{
    if (qi && qi->InterfaceType && RtlCompareMemory(qi->InterfaceType, &GUID_DEVINTERFACE_BRIGHTNESS, sizeof(GUID)) == sizeof(GUID)) {
        PDXGK_BRIGHTNESS_INTERFACE bi = (PDXGK_BRIGHTNESS_INTERFACE)qi->Interface;
        J606fLog("QueryInterface: brightness v%u size %u", qi->Version, qi->Size);
        if (qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE) || qi->Version < DXGK_BRIGHTNESS_INTERFACE_VERSION_1)
            return STATUS_NOT_SUPPORTED;
        bi->Size = sizeof(DXGK_BRIGHTNESS_INTERFACE);
        bi->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
        bi->Context = ctx;
        bi->InterfaceReference = BlRef;
        bi->InterfaceDereference = BlDeref;
        bi->GetPossibleBrightness = BlGetPossible;
        bi->SetBrightness = BlSet;
        bi->GetBrightness = BlGet;
        return STATUS_SUCCESS;
    }
    if (qi && qi->InterfaceType)
        J606fLog("QueryInterface: other %08lx-%04x v%u", qi->InterfaceType->Data1, qi->InterfaceType->Data2, qi->Version);
    return STATUS_NOT_SUPPORTED;
}
