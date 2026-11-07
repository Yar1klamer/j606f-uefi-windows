// J606F buttons as HID (Windows ARM64 kernel driver, test-signed): a WDM HID minidriver (HidRegisterMinidriver) for the root-enumerated
// device ROOT\J606FBTN\0000 (Enum\Root\J606FBTN\0000, Service j606fbtn; no ACPI device: FVMAIN of the UEFI has no room for SSDT changes).
// A system thread polls the PMIC PM6125 (SPMI SID 0) every 20 ms through the PMIC arbiter v5 observer channels of EE 0 (read commands only,
// as windrv/probe/probe.c and the UEFI AML battery): PON_RT_STS 0x0810 bit0 KPDPWR (Power), bit1 RESIN (Volume Down), 1 = pressed;
// GPIO5 RT_STS 0xC410 bit0 (Volume Up), 0 = pressed (checked in pmOS 2026-11-07, raw/buttons_regs_2026-11-07.txt).
// NOTHING is written to the PMIC: PON reset/S2/PS_HOLD settings stay as the boot loader left them (Power held 10-15 s = hardware reset).
// A state is taken after two equal reads in a row (debounce 20-40 ms). Reports: ID 1 Consumer Control (Volume Increment, Volume Decrement),
// ID 2 Generic Desktop System Control (System Power Down -> Windows power button action). Log: C:\j606f\out\hidbuttons.txt.
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
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\hidbuttons.txt");
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
    DbgPrint("j606f hidbuttons: %s", line);
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

// =====================================================================================================================================
// SPMI (PMIC arbiter v5, EE 0 observer channels), read only

#define SPMI_CORE_PA   0x01c40000
#define SPMI_OBS_PA    0x03e00000
#define REG_PON_RT     0x0810       // PM6125 PON_RT_STS: bit0 KPDPWR_N, bit1 RESIN_N
#define REG_GPIO5_RT   0xC410       // PM6125 GPIO5 RT_STS: bit0 input level

static volatile UCHAR *g_ch_pon, *g_ch_gpio;
static ULONG g_n_rd, g_n_err;

static volatile UCHAR *SpmiChannel(volatile UCHAR *core, USHORT sid, USHORT addr)
{
    USHORT ppid = (USHORT)((sid << 8) | (addr >> 8));
    ULONG n;

    for (n = 0; 0x900 + 4 * n < 0x1100; n++)
        if (((Rd(core, 0x900 + 4 * n) >> 8) & 0xfff) == ppid) {
            Log("SPMI ppid %03x -> apid %lu", ppid, (unsigned long)n);
            return Map(SPMI_OBS_PA + 0x80 * n, 0x80);
        }
    Log("SPMI ppid %03x: no apid", ppid);
    return NULL;
}

static int SpmiOpen(void)
{
    volatile UCHAR *core = Map(SPMI_CORE_PA, 0x2000);
    ULONG ver;

    if (!core)
        return -1;
    ver = Rd(core, 0);
    Log("SPMI arbiter version %08lx", (unsigned long)ver);
    if (ver >= 0x50000000 && ver < 0x70000000) {
        g_ch_pon = SpmiChannel(core, 0, REG_PON_RT);
        g_ch_gpio = SpmiChannel(core, 0, REG_GPIO5_RT);
    }
    MmUnmapIoSpace((PVOID)core, 0x2000);
    return g_ch_pon && g_ch_gpio ? 0 : -2;
}

static void SpmiClose(void)
{
    if (g_ch_pon)
        MmUnmapIoSpace((PVOID)g_ch_pon, 0x80);
    if (g_ch_gpio)
        MmUnmapIoSpace((PVOID)g_ch_gpio, 0x80);
    g_ch_pon = g_ch_gpio = NULL;
}

// one byte, PMIC_ARB_OP_EXT_READL; -1 on error/timeout
static int SpmiRead(volatile UCHAR *ch, USHORT addr)
{
    ULONG st = 0;
    int i;

    g_n_rd++;
    Wr(ch, 0x00, (1u << 27) | ((addr & 0xffu) << 4));
    for (i = 0; i < 2000; i++) {
        st = Rd(ch, 0x08);
        if (st & 1)
            break;
    }
    if ((st & 0xf) != 1) {
        if (g_n_err++ < 20)
            Log("SPMI read %04x: status %lx", addr, (unsigned long)st);
        return -1;
    }
    return (int)(Rd(ch, 0x18) & 0xff);
}

// =====================================================================================================================================
// HID: report descriptor and the read queue

#define REPLEN 2

static const UCHAR g_rdesc[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,             // Consumer, Consumer Control, Collection (Application)
    0x85, 0x01, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,   // Report ID 1, 1 bit
    0x09, 0xE9, 0x81, 0x02,                         // Volume Increment
    0x09, 0xEA, 0x81, 0x02,                         // Volume Decrement
    0x95, 0x06, 0x81, 0x03,                         // 6 bits padding
    0xC0,
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,             // Generic Desktop, System Control, Collection (Application)
    0x85, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01,   // Report ID 2, 1 bit
    0x09, 0x81, 0x81, 0x02,                         // System Power Down
    0x95, 0x07, 0x81, 0x03,                         // 7 bits padding
    0xC0
};

static KSPIN_LOCK g_lock;
static LIST_ENTRY g_reads;
#define RQ 16
static UCHAR g_ring[RQ][REPLEN];
static ULONG g_rhead, g_rcount;
static volatile LONG g_stop, g_running;
static PVOID g_thread;
static ULONG g_n_rep, g_n_done, g_n_drop, g_n_pend, g_n_cancel;

static void Copy(UCHAR *d, const UCHAR *s, ULONG n)
{
    while (n--)
        *d++ = *s++;
}

static void CompleteRead(PIRP irp, const UCHAR *rep)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);

    if (s->Parameters.DeviceIoControl.OutputBufferLength < REPLEN || !irp->UserBuffer) {
        irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        irp->IoStatus.Information = 0;
    } else {
        Copy((UCHAR *)irp->UserBuffer, rep, REPLEN);
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = REPLEN;
    }
    IofCompleteRequest(irp, IO_NO_INCREMENT);
}

// lock held: first queued read whose cancel routine we could take back, or NULL
static PIRP TakeRead(void)
{
    while (!IsListEmpty(&g_reads)) {
        PLIST_ENTRY e = RemoveHeadList(&g_reads);
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        InitializeListHead(e);          // the cancel routine may still RemoveEntryList it
        if (IoSetCancelRoutine(irp, NULL))
            return irp;
    }
    return NULL;
}

static VOID CancelRead(PDEVICE_OBJECT dev, PIRP irp)
{
    KIRQL q;

    UNREFERENCED_PARAMETER(dev);
    IoReleaseCancelSpinLock(irp->CancelIrql);
    q = KeAcquireSpinLockRaiseToDpc(&g_lock);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    InitializeListHead(&irp->Tail.Overlay.ListEntry);
    g_n_cancel++;
    KeReleaseSpinLock(&g_lock, q);
    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
}

static void QueueReport(const UCHAR *rep)
{
    KIRQL q = KeAcquireSpinLockRaiseToDpc(&g_lock);
    PIRP irp = TakeRead();

    g_n_rep++;
    if (!irp) {
        ULONG slot;
        if (g_rcount == RQ) {           // full: drop the oldest
            g_rhead = (g_rhead + 1) % RQ;
            g_rcount--;
            g_n_drop++;
        }
        slot = (g_rhead + g_rcount) % RQ;
        Copy(g_ring[slot], rep, REPLEN);
        g_rcount++;
        KeReleaseSpinLock(&g_lock, q);
        return;
    }
    g_n_done++;
    KeReleaseSpinLock(&g_lock, q);
    CompleteRead(irp, rep);
}

static NTSTATUS PendRead(PIRP irp)
{
    UCHAR rep[REPLEN];
    KIRQL q;

    if (IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength < REPLEN) {
        irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_BUFFER_TOO_SMALL;
    }
    q = KeAcquireSpinLockRaiseToDpc(&g_lock);
    if (g_rcount) {
        Copy(rep, g_ring[g_rhead], REPLEN);
        g_rhead = (g_rhead + 1) % RQ;
        g_rcount--;
        g_n_done++;
        KeReleaseSpinLock(&g_lock, q);
        CompleteRead(irp, rep);
        return STATUS_SUCCESS;
    }
    IoSetCancelRoutine(irp, CancelRead);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL)) {
        KeReleaseSpinLock(&g_lock, q);
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_CANCELLED;
    }
    IoMarkIrpPending(irp);
    InsertTailList(&g_reads, &irp->Tail.Overlay.ListEntry);
    g_n_pend++;
    KeReleaseSpinLock(&g_lock, q);
    return STATUS_PENDING;
}

static void FlushReads(NTSTATUS st)
{
    PIRP irp;
    KIRQL q;

    for (;;) {
        q = KeAcquireSpinLockRaiseToDpc(&g_lock);
        irp = TakeRead();
        KeReleaseSpinLock(&g_lock, q);
        if (!irp)
            break;
        irp->IoStatus.Status = st;
        irp->IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

// =====================================================================================================================================
// polling thread

#define B_PWR  1u
#define B_VDN  2u
#define B_VUP  4u

static ULONG g_logged;

// raw button bits, or -1 if a read failed
static int ReadButtons(void)
{
    int pon = SpmiRead(g_ch_pon, REG_PON_RT), gp = SpmiRead(g_ch_gpio, REG_GPIO5_RT);

    if (pon < 0 || gp < 0)
        return -1;
    return (pon & 1 ? B_PWR : 0) | (pon & 2 ? B_VDN : 0) | (gp & 1 ? 0 : B_VUP);
}

static void Send(ULONG id, ULONG bits)
{
    UCHAR rep[REPLEN];

    rep[0] = (UCHAR)id;
    rep[1] = (UCHAR)bits;
    QueueReport(rep);
}

static VOID ButtonThread(PVOID ctx)
{
    int last = -2, cur, raw;
    ULONG ticks = 0, stats = 0, shown = 0;

    UNREFERENCED_PARAMETER(ctx);
    Log("=== hidbuttons thread");
    if (SpmiOpen()) {
        Log("SPMI open failed; thread ends");
        goto out;
    }
    raw = ReadButtons();
    Log("initial: raw %d (PON %02x, GPIO5 %02x)", raw, SpmiRead(g_ch_pon, REG_PON_RT), SpmiRead(g_ch_gpio, REG_GPIO5_RT));
    cur = raw >= 0 ? raw : 0;
    shown = (ULONG)cur;
    last = raw;
    while (!g_stop) {
        SleepMs(20);
        raw = ReadButtons();
        if (raw >= 0 && raw == last && (ULONG)raw != shown) {   // two equal reads: accept
            ULONG chg = (ULONG)raw ^ shown;
            if (g_logged++ < 300)
                Log("buttons %lx -> %lx (power %u, vol- %u, vol+ %u)", (unsigned long)shown, (unsigned long)raw,
                    (raw & B_PWR) != 0, (raw & B_VDN) != 0, (raw & B_VUP) != 0);
            shown = (ULONG)raw;
            if (chg & (B_VUP | B_VDN))
                Send(1, (shown & B_VUP ? 1u : 0) | (shown & B_VDN ? 2u : 0));
            if (chg & B_PWR)
                Send(2, shown & B_PWR ? 1u : 0);
        }
        last = raw;
        if (++ticks >= 1500 && stats < 40) {   // ~30 s
            ticks = 0;
            stats++;
            Log("stats: reads %lu errors %lu reports %lu done %lu drop %lu pended %lu cancel %lu", (unsigned long)g_n_rd,
                (unsigned long)g_n_err, (unsigned long)g_n_rep, (unsigned long)g_n_done, (unsigned long)g_n_drop, (unsigned long)g_n_pend,
                (unsigned long)g_n_cancel);
        }
    }
out:
    SpmiClose();
    Log("hidbuttons thread exits");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS StartHw(void)
{
    HANDLE h;
    NTSTATUS st;

    if (g_thread)
        return STATUS_SUCCESS;
    g_stop = 0;
    st = PsCreateSystemThread(&h, 0x1FFFFF, NULL, NULL, NULL, ButtonThread, NULL);
    if (!NT_SUCCESS(st))
        return st;
    st = ObReferenceObjectByHandle(h, 0x1FFFFF, NULL, 0, &g_thread, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st))
        g_thread = NULL;
    return STATUS_SUCCESS;
}

static void StopHw(void)
{
    if (g_thread) {
        g_stop = 1;
        KeWaitForSingleObject(g_thread, 0, 0, FALSE, NULL);
        ObfDereferenceObject(g_thread);
        g_thread = NULL;
    }
    FlushReads(STATUS_DELETE_PENDING);
}

// =====================================================================================================================================
// dispatch

static PDEVICE_OBJECT NextDo(PDEVICE_OBJECT fdo)
{
    return ((PHID_DEVICE_EXTENSION)fdo->DeviceExtension)->NextDeviceObject;
}

static NTSTATUS Done(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IofCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS IoctlDispatch(PDEVICE_OBJECT fdo, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    ULONG out = s->Parameters.DeviceIoControl.OutputBufferLength;
    UCHAR *ub = (UCHAR *)irp->UserBuffer;

    UNREFERENCED_PARAMETER(fdo);
    switch (s->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        HID_DESCRIPTOR d;
        if (!ub || out < sizeof(d))
            return Done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        d.bLength = sizeof(d);
        d.bDescriptorType = 0x21;
        d.bcdHID = 0x0100;
        d.bCountry = 0;
        d.bNumDescriptors = 1;
        d.DescriptorList[0].bReportType = 0x22;
        d.DescriptorList[0].wReportLength = sizeof(g_rdesc);
        Copy(ub, (const UCHAR *)&d, sizeof(d));
        return Done(irp, STATUS_SUCCESS, sizeof(d));
    }
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        if (!ub || out < sizeof(g_rdesc))
            return Done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        Copy(ub, g_rdesc, sizeof(g_rdesc));
        return Done(irp, STATUS_SUCCESS, sizeof(g_rdesc));
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        HID_DEVICE_ATTRIBUTES a;
        ULONG i;
        if (!ub || out < sizeof(a))
            return Done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        a.Size = sizeof(a);
        a.VendorID = 0x17EF;
        a.ProductID = 0x606B;
        a.VersionNumber = 0x0001;
        for (i = 0; i < 11; i++)
            a.Reserved[i] = 0;
        Copy(ub, (const UCHAR *)&a, sizeof(a));
        return Done(irp, STATUS_SUCCESS, sizeof(a));
    }
    case IOCTL_HID_READ_REPORT:
        if (g_stop)
            return Done(irp, STATUS_DELETE_PENDING, 0);
        return PendRead(irp);
    case IOCTL_HID_GET_STRING: {
        static const WCHAR man[] = L"Lenovo", prod[] = L"J606F buttons (Power, Volume)";
        ULONG id = (ULONG)(ULONG_PTR)s->Parameters.DeviceIoControl.Type3InputBuffer & 0xffff;
        const WCHAR *str = id == HID_STRING_ID_IMANUFACTURER ? man : id == HID_STRING_ID_IPRODUCT ? prod : NULL;
        ULONG len = id == HID_STRING_ID_IMANUFACTURER ? sizeof(man) : sizeof(prod);
        if (!str)
            return Done(irp, STATUS_INVALID_PARAMETER, 0);
        if (!ub || out < len)
            return Done(irp, STATUS_BUFFER_TOO_SMALL, 0);
        Copy(ub, (const UCHAR *)str, len);
        return Done(irp, STATUS_SUCCESS, len);
    }
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        return Done(irp, STATUS_SUCCESS, 0);
    default:
        return Done(irp, STATUS_NOT_SUPPORTED, 0);
    }
}

static NTSTATUS SyncDone(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
    UNREFERENCED_PARAMETER(dev);
    UNREFERENCED_PARAMETER(irp);
    KeSetEvent((PKEVENT)ctx, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS PnpDispatch(PDEVICE_OBJECT fdo, PIRP irp)
{
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st;
    KEVENT ev;

    switch (s->MinorFunction) {
    case IRP_MN_START_DEVICE:
        KeInitializeEvent(&ev, 0, FALSE);
        IoCopyCurrentIrpStackLocationToNext(irp);
        IoSetCompletionRoutine(irp, SyncDone, &ev, 1, 1, 1);
        st = IofCallDriver(NextDo(fdo), irp);
        if (st == STATUS_PENDING)
            KeWaitForSingleObject(&ev, 0, 0, FALSE, NULL);
        st = irp->IoStatus.Status;
        Log("START_DEVICE: lower %08lx", (unsigned long)st);
        if (NT_SUCCESS(st))
            st = StartHw();
        irp->IoStatus.Status = st;
        IofCompleteRequest(irp, IO_NO_INCREMENT);
        return st;
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_REMOVE_DEVICE:
        Log("PnP minor %u: stopping", s->MinorFunction);
        StopHw();
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case 0x03:  /* CANCEL_REMOVE */
    case 0x05:  /* QUERY_STOP */
    case 0x06:  /* CANCEL_STOP */
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }
    IoSkipCurrentIrpStackLocation(irp);
    return IofCallDriver(NextDo(fdo), irp);
}

static NTSTATUS PowerDispatch(PDEVICE_OBJECT fdo, PIRP irp)
{
    PoStartNextPowerIrp(irp);
    IoSkipCurrentIrpStackLocation(irp);
    return PoCallDriver(NextDo(fdo), irp);
}

static NTSTATUS AddDevice(PDRIVER_OBJECT drv, PDEVICE_OBJECT fdo)
{
    UNREFERENCED_PARAMETER(drv);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    Log("AddDevice: fdo %p next %p", fdo, NextDo(fdo));
    return STATUS_SUCCESS;
}

static VOID HidUnload(PDRIVER_OBJECT drv)
{
    UNREFERENCED_PARAMETER(drv);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    HID_MINIDRIVER_REGISTRATION r;
    NTSTATUS st;

    Log("=== j606f hidbuttons: DriverEntry");
    g_lock = 0;
    InitializeListHead(&g_reads);
    drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = IoctlDispatch;
    drv->MajorFunction[IRP_MJ_PNP] = PnpDispatch;
    drv->MajorFunction[IRP_MJ_POWER] = PowerDispatch;
    ((PDRIVER_EXTENSION)drv->DriverExtension)->AddDevice = AddDevice;
    drv->DriverUnload = HidUnload;

    r.Revision = HID_REVISION;
    r.DriverObject = drv;
    r.RegistryPath = reg;
    r.DeviceExtensionSize = 16;
    r.DevicesArePolled = FALSE;
    r.Reserved[0] = r.Reserved[1] = r.Reserved[2] = 0;
    st = HidRegisterMinidriver(&r);
    Log("HidRegisterMinidriver: %08lx", (unsigned long)st);
    return st;
}
