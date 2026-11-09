// J606F USB-C (Windows ARM64 kernel driver, test-signed, legacy service, no INF).
// In Linux the USB host works through the FUSB302B (I2C0 0x22, INT gpio97): DRP toggling finds Rd on CC (a device),
// tcpm turns on the external VBUS boost (TLMM gpio38, regulator usb_otg_vbus) and switches dwc3 to host.
// In Windows nobody does that: URS0 is in host mode, but there is no VBUS and no CC termination -> nothing is attached (run_34).
// I2C0 (QUP0 SE0) works only through GPI DMA in Windows (run_xx, LOG) -> this driver bit-bangs I2C on the same pins
// (gpio0 SDA, gpio1 SCL, TLMM west tile) with open-drain emulation (OE on = drive 0, OE off = pull-up). Nothing else in Windows uses I2C0.
//
// Modes (service name):
//   ...usbr  read only: TLMM gpio0/1/38/41/97 cfg+io, dwc3/xHCI registers, PMI632 USBIN (SPMI), FUSB302 registers (I2C reads only;
//            the gpio0/1 pin configuration is restored at the end).
//   ...usbc  same dump, then a system thread (50 ms) keeps a minimal Type-C state machine:
//            TOGGLING (DRP) -> TOGDONE SRC_CCx (Rd seen = a device) -> Rp + MEAS on that CC, debounce 200 ms, VBUSOK must be 0
//            (no foreign VBUS) -> gpio38 = 1 (VBUS boost).  Detach = COMP high (CC above Rd threshold) twice -> gpio38 = 0 -> TOGGLING.
//            TOGDONE SNK_CCx (Rp seen = Mac / charger) -> Rd on both CC, gpio38 stays 0; when VBUSOK drops -> TOGGLING.
//            gpio38 is never set while VBUSOK=1 before our own VBUS; on unload gpio38 = 0.
// Never touched: SMB1355 (I2C1), PMIC writes (SPMI is read only), boot/calibration partitions.
// Log: C:\j606f\out\usbc.txt.
#include "j606f_wdm.h"

void KeStallExecutionProcessor(ULONG MicroSeconds);

static void LogRaw(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\usbc.txt");
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
    DbgPrint("j606f usbc: %s", line);
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
// TLMM (sm6115: west 0x500000, south 0x900000, east 0xd00000; ctl = 0x1000 * gpio, io = +4)
// cfg: bits 1:0 pull (0 none, 1 down, 3 up), 5:2 func, 8:6 drive ((mA/2)-1), 9 OE.  io: bit0 in, bit1 out.

#define TLMM_WEST_PA 0x00500000
#define TLMM_EAST_PA 0x00d00000

static volatile UCHAR *g_sda, *g_scl, *g_vbus, *g_g41, *g_g97;
static ULONG g_sda_cfg0, g_scl_cfg0, g_sda_io0, g_scl_io0;

static void PinDump(const char *name, volatile UCHAR *p)
{
    ULONG c, io;

    if (!p)
        return;
    c = Rd(p, 0);
    io = Rd(p, 4);
    Log("%s cfg %03lx (pull %lu func %lu drv %lumA oe %lu) io %lx", name, (unsigned long)c, (unsigned long)(c & 3),
        (unsigned long)((c >> 2) & 0xf), (unsigned long)((((c >> 6) & 7) + 1) * 2), (unsigned long)((c >> 9) & 1), (unsigned long)io);
}

// =====================================================================================================================================
// bit-banged I2C (~100 kHz), open drain by OE: low = OE 1 + out 0; release = OE 0 (pull-up)

#define CFG_GPIO_PU_2MA     0x003   // func 0, pull up, 2 mA, OE 0
#define CFG_GPIO_PU_2MA_OE  0x203
#define T_HALF 5

static void LineLow(volatile UCHAR *p)
{
    Wr(p, 4, 0);
    Wr(p, 0, CFG_GPIO_PU_2MA_OE);
}

static void LineRel(volatile UCHAR *p)
{
    Wr(p, 0, CFG_GPIO_PU_2MA);
}

static int LineIn(volatile UCHAR *p)
{
    return (int)(Rd(p, 4) & 1);
}

static int SclRelWait(void)
{
    int i;

    LineRel(g_scl);
    for (i = 0; i < 1000 && !LineIn(g_scl); i++)
        KeStallExecutionProcessor(1);       // clock stretching
    KeStallExecutionProcessor(T_HALF);
    return LineIn(g_scl) ? 0 : -1;
}

static void I2cStart(void)
{
    LineRel(g_sda);
    SclRelWait();
    LineLow(g_sda);
    KeStallExecutionProcessor(T_HALF);
    LineLow(g_scl);
    KeStallExecutionProcessor(T_HALF);
}

static void I2cStop(void)
{
    LineLow(g_sda);
    KeStallExecutionProcessor(T_HALF);
    SclRelWait();
    LineRel(g_sda);
    KeStallExecutionProcessor(T_HALF);
}

static int I2cTx(UCHAR b)               // 0 = ACK
{
    int i, ack;

    for (i = 7; i >= 0; i--) {
        if (b & (1 << i))
            LineRel(g_sda);
        else
            LineLow(g_sda);
        KeStallExecutionProcessor(T_HALF);
        SclRelWait();
        LineLow(g_scl);
    }
    LineRel(g_sda);
    KeStallExecutionProcessor(T_HALF);
    SclRelWait();
    ack = LineIn(g_sda);
    LineLow(g_scl);
    KeStallExecutionProcessor(T_HALF);
    return ack;
}

static UCHAR I2cRx(int last)
{
    int i;
    UCHAR b = 0;

    LineRel(g_sda);
    for (i = 7; i >= 0; i--) {
        KeStallExecutionProcessor(T_HALF);
        SclRelWait();
        if (LineIn(g_sda))
            b |= (UCHAR)(1 << i);
        LineLow(g_scl);
    }
    if (last)
        LineRel(g_sda);
    else
        LineLow(g_sda);
    KeStallExecutionProcessor(T_HALF);
    SclRelWait();
    LineLow(g_scl);
    LineRel(g_sda);
    KeStallExecutionProcessor(T_HALF);
    return b;
}

#define FUSB_ADDR 0x22
static ULONG g_i2c_err;

static int FusbRead(UCHAR reg)          // -1 on NACK
{
    int v = -1;

    I2cStart();
    if (I2cTx(FUSB_ADDR << 1) == 0 && I2cTx(reg) == 0) {
        I2cStart();
        if (I2cTx((FUSB_ADDR << 1) | 1) == 0)
            v = I2cRx(1);
    }
    I2cStop();
    if (v < 0 && g_i2c_err++ < 20)
        Log("fusb read %02x: NACK", reg);
    return v;
}

static int g_allow_write;

static int FusbWrite(UCHAR reg, UCHAR val)
{
    int ok = 0;

    if (!g_allow_write) {
        Log("fusb write %02x blocked (read-only mode)", reg);
        return -1;
    }
    I2cStart();
    if (I2cTx(FUSB_ADDR << 1) == 0 && I2cTx(reg) == 0 && I2cTx(val) == 0)
        ok = 1;
    I2cStop();
    if (!ok && g_i2c_err++ < 20)
        Log("fusb write %02x=%02x: NACK", reg, val);
    return ok ? 0 : -1;
}

// FUSB302 registers
#define F_DEVICE_ID  0x01
#define F_SWITCHES0  0x02   // b7 PU_EN2, b6 PU_EN1, b5 VCONN_CC2, b4 VCONN_CC1, b3 MEAS_CC2, b2 MEAS_CC1, b1 PDWN2, b0 PDWN1
#define F_SWITCHES1  0x03
#define F_MEASURE    0x04   // b6 MEAS_VBUS, 5:0 MDAC
#define F_CONTROL0   0x06   // 3:2 HOST_CUR (1 = default 80 uA), b5 INT_MASK
#define F_CONTROL1   0x07
#define F_CONTROL2   0x08   // 2:1 MODE (1 DRP, 2 SNK, 3 SRC), b0 TOGGLE
#define F_CONTROL3   0x09
#define F_MASK       0x0A
#define F_POWER      0x0B
#define F_RESET      0x0C   // b0 SW_RES
#define F_MASKA      0x0E
#define F_MASKB      0x0F
#define F_STATUS0A   0x3C
#define F_STATUS1A   0x3D   // 5:3 TOGSS: 1 SRC_CC1, 2 SRC_CC2, 5 SNK_CC1, 6 SNK_CC2, 7 audio
#define F_INTA       0x3E   // b6 TOGDONE
#define F_INTB       0x3F
#define F_STATUS0    0x40   // b7 VBUSOK, b5 COMP, 1:0 BC_LVL
#define F_STATUS1    0x41
#define F_INT        0x42

#define MDAC_RD_DEFAULT 38   // 1638 mV: CC above this with default Rp = open (no Rd)

static void FusbDump(void)
{
    static const UCHAR regs[] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0E, 0x0F, 0x3C, 0x3D, 0x40, 0x41 };
    char line[160];
    int i, n = 0, v;

    for (i = 0; i < (int)sizeof(regs); i++) {
        v = FusbRead(regs[i]);
        n += Fmt(line + n, sizeof(line) - n - 1, " %02x=%s%02x", regs[i], v < 0 ? "!" : "", v < 0 ? 0 : v);
        if (n > (int)sizeof(line) - 12)
            break;
    }
    line[sizeof(line) - 1] = 0;
    Log("fusb:%s", line);
}

// =====================================================================================================================================
// SPMI read only (PMIC arbiter v5, EE 0 observer channels; as hidbuttons.c): PMI632 (SID 2) USBIN_INT_RT_STS 0x1310 bit4 = plugged in

#define SPMI_CORE_PA   0x01c40000
#define SPMI_OBS_PA    0x03e00000

static volatile UCHAR *SpmiChannel(USHORT sid, USHORT addr)
{
    volatile UCHAR *core = Map(SPMI_CORE_PA, 0x2000), *ch = NULL;
    USHORT ppid = (USHORT)((sid << 8) | (addr >> 8));
    ULONG n, ver;

    if (!core)
        return NULL;
    ver = Rd(core, 0);
    if (ver >= 0x50000000 && ver < 0x70000000) {
        for (n = 0; 0x900 + 4 * n < 0x1100; n++)
            if (((Rd(core, 0x900 + 4 * n) >> 8) & 0xfff) == ppid) {
                ch = Map(SPMI_OBS_PA + 0x80 * n, 0x80);
                break;
            }
    }
    if (!ch)
        Log("SPMI ppid %03x: no channel (arbiter %08lx)", ppid, (unsigned long)ver);
    MmUnmapIoSpace((PVOID)core, 0x2000);
    return ch;
}

static int SpmiRead(volatile UCHAR *ch, USHORT addr)
{
    ULONG st = 0;
    int i;

    if (!ch)
        return -1;
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

static volatile UCHAR *g_usbin;

// =====================================================================================================================================
// dwc3 / xHCI dump (controller is clocked: usbxhci runs on it)

#define DWC3_PA 0x04e00000

static void UsbDump(void)
{
    volatile UCHAR *x = Map(DWC3_PA, 0x10000), *q = Map(0x04ef8800, 0x400);
    ULONG cap, op, i, nports;

    if (x) {
        cap = Rd(x, 0) & 0xff;
        op = cap;
        nports = (Rd(x, 4) >> 24) & 0xff;
        Log("xhci caplen %lx hcsparams1 %08lx usbcmd %08lx usbsts %08lx", (unsigned long)cap, (unsigned long)Rd(x, 4),
            (unsigned long)Rd(x, op), (unsigned long)Rd(x, op + 4));
        for (i = 0; i < nports && i < 4; i++)
            Log("portsc%lu %08lx", (unsigned long)(i + 1), (unsigned long)Rd(x, op + 0x400 + 0x10 * i));
        Log("dwc3 GSNPSID %08lx GCTL %08lx GSTS %08lx GUSB2PHYCFG %08lx GUSB3PIPECTL %08lx GDBGLTSSM %08lx",
            (unsigned long)Rd(x, 0xc120), (unsigned long)Rd(x, 0xc110), (unsigned long)Rd(x, 0xc118), (unsigned long)Rd(x, 0xc200),
            (unsigned long)Rd(x, 0xc2c0), (unsigned long)Rd(x, 0xc164));
        MmUnmapIoSpace((PVOID)x, 0x10000);
    }
    if (q) {
        char line[200];
        int n = 0;
        for (i = 0; i < 0x40; i += 4)
            n += Fmt(line + n, sizeof(line) - n - 1, " %02lx=%08lx", (unsigned long)i, (unsigned long)Rd(q, i));
        line[sizeof(line) - 1] = 0;
        Log("qscratch:%s", line);
        MmUnmapIoSpace((PVOID)q, 0x400);
    }
}

// =====================================================================================================================================
// VBUS boost gpio38

static int g_vbus_on;

static void VbusSet(int on)
{
    if (!g_vbus || !g_allow_write)
        return;
    if (on) {
        Wr(g_vbus, 4, 2);                   // out = 1
        Wr(g_vbus, 0, 0x200);               // func 0, 2 mA, no pull, OE
    } else {
        Wr(g_vbus, 4, 0);
        Wr(g_vbus, 0, 0x200);
    }
    g_vbus_on = on;
    Log("gpio38 VBUS boost := %d (io %lx)", on, (unsigned long)Rd(g_vbus, 4));
}

// =====================================================================================================================================
// Type-C state machine (thread)

enum { ST_TOGGLE, ST_SRC_DEB, ST_SRC, ST_SNK };
static volatile LONG g_stop;
static PVOID g_thread;
static int g_state, g_cc, vbwait;

static void StartToggling(void)
{
    VbusSet(0);
    FusbWrite(F_CONTROL2, 0x00);
    FusbWrite(F_SWITCHES0, 0x00);           // no pulls, toggling logic drives them
    FusbWrite(F_CONTROL0, 0x04);            // HOST_CUR default (80 uA), interrupts unmasked
    FusbWrite(F_MEASURE, MDAC_RD_DEFAULT);
    FusbWrite(F_POWER, 0x07);
    FusbRead(F_INTA);                       // clear
    FusbRead(F_INT);
    FusbWrite(F_CONTROL2, 0x03);            // MODE DRP, TOGGLE
    g_state = ST_TOGGLE;
    Log("state TOGGLING");
}

static int UsbinPlugged(void)
{
    int v = SpmiRead(g_usbin, 0x1310);
    return v < 0 ? -1 : ((v >> 4) & 1);
}

static VOID UsbcThread(PVOID ctx)
{
    int deb = 0, miss = 0, last_s0 = -1;
    ULONG ticks = 0;

    UNREFERENCED_PARAMETER(ctx);
    StartToggling();
    while (!g_stop) {
        int s0 = FusbRead(F_STATUS0), s1a, ia, togss;

        if (s0 < 0) {
            SleepMs(200);
            continue;
        }
        if (s0 != last_s0 && g_state != ST_TOGGLE)
            Log("status0 %02x (vbusok %d comp %d bc %d) state %d", s0, (s0 >> 7) & 1, (s0 >> 5) & 1, s0 & 3, g_state);
        last_s0 = s0;
        switch (g_state) {
        case ST_TOGGLE:
            ia = FusbRead(F_INTA);
            if (ia > 0 && (ia & 0x40)) {
                s1a = FusbRead(F_STATUS1A);
                togss = s1a < 0 ? 0 : (s1a >> 3) & 7;
                Log("TOGDONE togss %d status0 %02x usbin %d", togss, s0, UsbinPlugged());
                FusbWrite(F_CONTROL2, 0x00);    // stop toggling
                if (togss == 1 || togss == 2) {
                    g_cc = togss;
                    FusbWrite(F_SWITCHES0, g_cc == 1 ? 0x44 : 0x88);   // PU_ENx + MEAS_CCx
                    FusbWrite(F_MEASURE, MDAC_RD_DEFAULT);
                    deb = 0;
                    g_state = ST_SRC_DEB;
                    Log("state SRC_DEBOUNCE cc%d", g_cc);
                } else if (togss == 5 || togss == 6) {
                    g_cc = togss - 4;
                    FusbWrite(F_SWITCHES0, g_cc == 1 ? 0x07 : 0x0B);   // PDWN1|PDWN2 + MEAS_CCx
                    g_state = ST_SNK;
                    Log("state SNK cc%d (partner is a source: Mac/charger) - VBUS boost stays off", g_cc);
                } else {
                    StartToggling();
                }
            }
            break;
        case ST_SRC_DEB:
            if (s0 & 0x20) {                    // COMP high: no Rd any more
                Log("SRC debounce: Rd lost");
                StartToggling();
            } else if (++deb >= 4 && (s0 & 0x80) && ++vbwait < 40) {
                ;                               // our own VBUS may still be decaying after a detach: wait up to 2 s
            } else if (deb >= 4) {              // 200 ms
                vbwait = 0;
                if (s0 & 0x80) {
                    Log("SRC: foreign VBUS present (vbusok=1, usbin %d) - boost NOT enabled", UsbinPlugged());
                    g_state = ST_SRC;          // keep Rp, track detach, no VBUS from us
                } else {
                    VbusSet(1);
                    g_state = ST_SRC;
                }
                miss = 0;
                Log("state SRC_ATTACHED cc%d vbus_on %d", g_cc, g_vbus_on);
            }
            break;
        case ST_SRC:
            if (s0 & 0x20) {
                if (++miss >= 2) {
                    Log("SRC: detach");
                    StartToggling();
                }
            } else {
                miss = 0;
            }
            break;
        case ST_SNK:
            if (!(s0 & 0x80)) {
                if (++miss >= 4) {
                    Log("SNK: VBUS gone - detach");
                    miss = 0;
                    StartToggling();
                }
            } else {
                miss = 0;
            }
            break;
        }
        if ((++ticks % 1200) == 0)              // once a minute
            Log("alive state %d vbus_on %d status0 %02x usbin %d", g_state, g_vbus_on, s0, UsbinPlugged());
        SleepMs(50);
    }
    VbusSet(0);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// =====================================================================================================================================

static int g_active;

static VOID UsbcUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_thread) {
        g_stop = 1;
        KeWaitForSingleObject(g_thread, 0, 0, FALSE, NULL);
        ObfDereferenceObject(g_thread);
        g_thread = NULL;
    }
    if (g_vbus) {
        VbusSet(0);
        PinDump("gpio38 at unload", g_vbus);
    }
    Log("=== unload");
}

// service name ends with "usbc" -> active mode
static int ServiceIsActive(PUNICODE_STRING path)
{
    USHORT n = path->Length / sizeof(WCHAR);
    PWCH s = path->Buffer;

    return n >= 4 && (s[n - 4] | 0x20) == 'u' && (s[n - 3] | 0x20) == 's' && (s[n - 2] | 0x20) == 'b' && (s[n - 1] | 0x20) == 'c';
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    int id, plug;

    DriverObject->DriverUnload = UsbcUnload;
    g_active = RegistryPath && RegistryPath->Buffer && ServiceIsActive(RegistryPath);
    Log("=== j606f usbc start, mode %s", g_active ? "ACTIVE (FUSB302 DRP + VBUS gpio38)" : "read-only");

    g_sda = Map(TLMM_WEST_PA + 0x1000 * 0, 0x1000);
    g_scl = Map(TLMM_WEST_PA + 0x1000 * 1, 0x1000);
    g_vbus = Map(TLMM_EAST_PA + 0x1000 * 38, 0x1000);
    g_g41 = Map(TLMM_EAST_PA + 0x1000 * 41, 0x1000);
    g_g97 = Map(TLMM_WEST_PA + 0x1000 * 97, 0x1000);
    PinDump("gpio0 sda", g_sda);
    PinDump("gpio1 scl", g_scl);
    PinDump("gpio38 vbus", g_vbus);
    PinDump("gpio41 mcu_switch", g_g41);
    PinDump("gpio97 fusb int", g_g97);

    g_usbin = SpmiChannel(2, 0x1310);
    plug = UsbinPlugged();
    Log("PMI632 USBIN_INT_RT_STS %d -> usbin plugged %d", SpmiRead(g_usbin, 0x1310), plug);
    UsbDump();

    if (!g_sda || !g_scl)
        return STATUS_SUCCESS;
    g_sda_cfg0 = Rd(g_sda, 0);
    g_scl_cfg0 = Rd(g_scl, 0);
    g_sda_io0 = Rd(g_sda, 4);
    g_scl_io0 = Rd(g_scl, 4);
    LineRel(g_sda);
    LineRel(g_scl);
    KeStallExecutionProcessor(50);
    Log("bus idle: sda %d scl %d", LineIn(g_sda), LineIn(g_scl));
    if (!LineIn(g_sda)) {                       // a stuck slave: 9 clocks
        int i;
        for (i = 0; i < 9 && !LineIn(g_sda); i++) {
            LineLow(g_scl);
            KeStallExecutionProcessor(T_HALF);
            SclRelWait();
        }
        I2cStop();
        Log("bus recovery: sda %d", LineIn(g_sda));
    }
    id = FusbRead(F_DEVICE_ID);
    Log("FUSB302 DEVICE_ID %02x (expect 0x9x)", id);
    FusbDump();

    if (!g_active || id < 0 || (id & 0xf0) != 0x90) {
        Wr(g_sda, 0, g_sda_cfg0);               // restore qup0 pin configuration
        Wr(g_scl, 0, g_scl_cfg0);
        Log("read-only done, pins restored (cfg %03lx %03lx)", (unsigned long)Rd(g_sda, 0), (unsigned long)Rd(g_scl, 0));
        return STATUS_SUCCESS;
    }

    g_allow_write = 1;
    {
        HANDLE h;
        NTSTATUS st = PsCreateSystemThread(&h, 0x1FFFFF, NULL, NULL, NULL, UsbcThread, NULL);
        if (NT_SUCCESS(st)) {
            if (!NT_SUCCESS(ObReferenceObjectByHandle(h, 0x1FFFFF, NULL, 0, &g_thread, NULL)))
                g_thread = NULL;
            ZwClose(h);
        }
        Log("thread %08lx", (unsigned long)st);
    }
    return STATUS_SUCCESS;
}
