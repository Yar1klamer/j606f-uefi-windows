// J606F: hardware probe for Windows. Logs the state left by the firmware (GCC clocks of QUP0, TLMM gpio0/1, GENI SE0 = i2c0)
// to C:\j606f\out\probe.txt, plus the PMI632 charger registers read over SPMI (both modes). Mode comes from the service name:
//   j606fprobe — read only (SE0 registers only if its clocks are already on);
//   j606fi2c   — also turns on the SE0 clocks/pins, sets SE0 up like Linux i2c-qcom-geni (FIFO mode, 100 kHz from 19.2 MHz)
//                and reads the mm8013 fuel gauge (0x55) word registers. Nothing is written to the gauge.
// Addresses and sequences: linux-sm61x5 gcc-sm6115.c, pinctrl-sm6115.c, sm6115.dtsi, qcom-geni-se.c, i2c-qcom-geni.c, mm8013.c.
#include "j606f_nt.h"

#define GCC_VOTE_PA     0x01479000  // GCC 0x7900c: APCS clock branch enable vote
#define GCC_QUP_PA      0x0141f000  // GCC 0x1f0xx: QUP wrapper 0 branches, 0x1f148 SE0 RCGR
#define TLMM_GPIO0_PA   0x00500000  // TLMM "west" tile, ctl_reg = 0x1000 * gpio
#define QUP0_WRAP_PA    0x04ac0000
#define SE0_PA          0x04a80000
#define SE_SIZE         0x4000

#define VOTE_REG        0x00c
#define VOTE_BITS       ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10))  // m_ahb, s_ahb, core, core_2x, s0
#define CBCR_OFF        (1u << 31)

// GENI SE registers
#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define SE_GENI_CGC_CTRL        0x28
#define SE_GENI_STATUS          0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_IF_DISABLE_RO      0x64
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_I2C_TX_TRANS_LEN     0x26c
#define SE_I2C_RX_TRANS_LEN     0x270
#define SE_I2C_SCL_COUNTERS     0x278
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WM       0x814
#define SE_GENI_IOS             0x908
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL_CFG      0xe30

#define M_CMD_DONE        (1u << 0)
#define M_CMD_FAILURE     (1u << 3)
#define M_CMD_ABORT       (1u << 5)
#define M_GP_IRQ_1        (1u << 10)  // NACK
#define M_GP_IRQ_3        (1u << 12)  // bus protocol error
#define M_GP_IRQ_4        (1u << 13)  // arbitration lost
#define M_RX_FIFO_WM      (1u << 26)
#define M_RX_FIFO_LAST    (1u << 27)
#define M_TX_FIFO_WM      (1u << 30)
#define M_COMMON_IRQ_EN   (0x7eu | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define M_I2C_ERRORS      (M_CMD_FAILURE | M_CMD_ABORT | M_GP_IRQ_1 | M_GP_IRQ_3 | M_GP_IRQ_4 | (1u << 1) | (1u << 2))

#define I2C_WRITE      1
#define I2C_READ       2
#define STOP_STRETCH   (1u << 2)
#define MM8013_ADDR    0x55

static int g_active;

static void LogRaw(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\probe.txt");
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    // write-through: if a register access hangs the machine, the lines before it are already on disk
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

    __builtin_va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 3, fmt, ap);
    __builtin_va_end(ap);
    line[sizeof(line) - 3] = 0;
    {
        ULONG n = 0;
        while (line[n])
            n++;
        line[n] = '\r';
        line[n + 1] = '\n';
        line[n + 2] = 0;
    }
    DbgPrint("j606f probe: %s", line);
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

// Wait until (status & mask) != 0; returns the last status. Bounded: ~200 ms.
static ULONG WaitIrq(volatile UCHAR *se, ULONG mask)
{
    ULONG st = 0;
    int i;

    for (i = 0; i < 2000; i++) {
        st = Rd(se, SE_GENI_M_IRQ_STATUS);
        if (st & (mask | M_I2C_ERRORS))
            return st;
        if (i >= 1000)
            SleepMs(1);
    }
    return st;
}

static void AbortCmd(volatile UCHAR *se)
{
    ULONG st;

    Wr(se, SE_GENI_M_CMD_CTRL_REG, 1u << 1);  // M_GENI_CMD_ABORT
    st = WaitIrq(se, M_CMD_ABORT);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    Log("  abort: m_irq %08lx status %08lx", (unsigned long)st, (unsigned long)Rd(se, SE_GENI_STATUS));
}

// SMBus "read word": write the register number with a stretched stop, then read 2 bytes (two messages, as Linux does).
static int ReadWord(volatile UCHAR *se, UCHAR reg, ULONG *out)
{
    ULONG st, rx, word;

    if (Rd(se, SE_GENI_STATUS) & 1) {
        Log("  reg %02x: M command still active, abort", reg);
        AbortCmd(se);
        return -1;
    }
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    Wr(se, SE_I2C_TX_TRANS_LEN, 1);
    Wr(se, SE_GENI_M_CMD0, (I2C_WRITE << 27) | (MM8013_ADDR << 9) | STOP_STRETCH);
    Wr(se, SE_GENI_TX_WATERMARK, 1);
    st = WaitIrq(se, M_TX_FIFO_WM);
    if (!(st & M_TX_FIFO_WM) || (st & M_I2C_ERRORS)) {
        Log("  reg %02x: no TX watermark, m_irq %08lx", reg, (unsigned long)st);
        Wr(se, SE_GENI_TX_WATERMARK, 0);
        AbortCmd(se);
        return -1;
    }
    Wr(se, SE_GENI_TX_FIFOn, reg);
    Wr(se, SE_GENI_TX_WATERMARK, 0);
    st = WaitIrq(se, M_CMD_DONE);
    Wr(se, SE_GENI_M_IRQ_CLEAR, st);
    if (!(st & M_CMD_DONE) || (st & M_I2C_ERRORS)) {
        Log("  reg %02x: write failed, m_irq %08lx%s", reg, (unsigned long)st, (st & M_GP_IRQ_1) ? " (NACK)" : "");
        if (!(st & M_CMD_DONE))
            AbortCmd(se);
        return -1;
    }

    Wr(se, SE_I2C_RX_TRANS_LEN, 2);
    Wr(se, SE_GENI_M_CMD0, (I2C_READ << 27) | (MM8013_ADDR << 9));
    st = WaitIrq(se, M_CMD_DONE);
    rx = Rd(se, SE_GENI_RX_FIFO_STATUS);
    if ((st & M_I2C_ERRORS) || !(rx & 0x1ffffff)) {
        Log("  reg %02x: read failed, m_irq %08lx rx_status %08lx", reg, (unsigned long)st, (unsigned long)rx);
        Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
        if (!(st & M_CMD_DONE))
            AbortCmd(se);
        return -1;
    }
    word = Rd(se, SE_GENI_RX_FIFOn);
    if (!(st & M_CMD_DONE))
        st |= WaitIrq(se, M_CMD_DONE);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    *out = word & 0xffff;  // little endian: first byte in bits 7:0
    return 0;
}

static void DumpSe(volatile UCHAR *se, volatile UCHAR *wrap)
{
    static const USHORT regs[] = { GENI_FORCE_DEFAULT_REG, GENI_OUTPUT_CTRL, SE_GENI_CGC_CTRL, SE_GENI_STATUS, GENI_SER_M_CLK_CFG,
                                   GENI_IF_DISABLE_RO, GENI_FW_REVISION_RO, SE_GENI_CLK_SEL, SE_GENI_BYTE_GRAN, SE_GENI_DMA_MODE_EN,
                                   SE_GENI_TX_PACKING_CFG0, SE_GENI_TX_PACKING_CFG1, SE_I2C_SCL_COUNTERS, SE_GENI_RX_PACKING_CFG0,
                                   SE_GENI_RX_PACKING_CFG1, SE_GENI_M_IRQ_STATUS, SE_GENI_M_IRQ_EN, SE_GENI_TX_FIFO_STATUS,
                                   SE_GENI_RX_FIFO_STATUS, SE_GENI_TX_WATERMARK, SE_GENI_RX_WATERMARK, SE_GENI_RX_RFR_WM, SE_GENI_IOS,
                                   SE_GSI_EVENT_EN, SE_IRQ_EN, SE_HW_PARAM_0, SE_HW_PARAM_1, SE_DMA_GENERAL_CFG };
    unsigned i;

    Log("QUP0 wrapper: HW_VER(0x4) %08lx", (unsigned long)Rd(wrap, 0x4));
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        Log("SE0 +%03x = %08lx", regs[i], (unsigned long)Rd(se, regs[i]));
}

// PMI632 charger (SMB5, SPMI sid 2) registers through the PMIC arbiter v5 observer channel of EE 0, as spmi-pmic-arb.c reads them.
// Only read commands; the PMIC itself is not written. Same register list as scripts/pmos_charger_status.sh.
#define SPMI_CORE_PA   0x01c40000
#define SPMI_OBS_PA    0x03e00000

static int SpmiRead(volatile UCHAR *core, USHORT sid, USHORT addr, ULONG *out)
{
    USHORT ppid = (USHORT)((sid << 8) | (addr >> 8));
    volatile UCHAR *ch;
    ULONG n, st = 0;
    int i, apid = -1;

    for (n = 0; 0x900 + 4 * n < 0x1100; n++) {
        if (((Rd(core, 0x900 + 4 * n) >> 8) & 0xfff) == ppid) {
            apid = (int)n;
            break;
        }
    }
    if (apid < 0)
        return -2;
    ch = Map(SPMI_OBS_PA + 0x80 * (ULONG)apid, 0x80);
    if (!ch)
        return -3;
    Wr(ch, 0x00, (1u << 27) | ((addr & 0xffu) << 4));  // PMIC_ARB_OP_EXT_READL, 1 byte
    for (i = 0; i < 1000; i++) {
        st = Rd(ch, 0x08);
        if (st & 1)
            break;
        if (i >= 500)
            SleepMs(1);
    }
    if ((st & 0xf) == 1)
        *out = Rd(ch, 0x18) & 0xff;
    MmUnmapIoSpace((PVOID)ch, 0x80);
    return (st & 0xf) == 1 ? 0 : -(int)(0x100 | st);
}

// PMI632 QG (qgauge @ 0x4800, SDAM @ 0xb100): battery voltage/current measured by the PMIC itself, for a Windows battery without I2C0.
// Register map and scaling from the Lenovo kernel drivers/power/supply/qcom/qg-reg.h, qg-defs.h, qg-util.c (read-only here).
static int SpmiRead16(volatile UCHAR *core, USHORT addr, ULONG *out)
{
    ULONG lo = 0, hi = 0;
    int rc = SpmiRead(core, 2, addr, &lo);
    if (rc == 0)
        rc = SpmiRead(core, 2, (USHORT)(addr + 1), &hi);
    *out = lo | (hi << 8);
    return rc;
}

static void QgDump(volatile UCHAR *core)
{
    static const struct { USHORT reg; const char *name; } regs[] = {
        { 0x4804, "QG TYPE" }, { 0x4805, "QG SUBTYPE (3 = IBAT 5A, 4 = 10A)" }, { 0x4808, "QG STATUS1 (bit7 QG_OK, bit0 BATT_PRESENT)" },
        { 0x4809, "QG STATUS2 (bit3 BATT_MISSING)" }, { 0x480a, "QG STATUS3" }, { 0x480b, "QG STATUS4" }, { 0x4810, "QG INT_RT_STS" },
        { 0x4843, "QG MODE_CTL1" }, { 0x4844, "QG MODE_CTL2" }, { 0x4846, "QG 0x46 (enable)" }, { 0x48bf, "QG SOC_MONOTONIC (x100/255, written by the OS driver)" },
        { 0xb146, "SDAM QG valid" }, { 0xb147, "SDAM QG SOC % (written by the OS driver)" },
    };
    static const struct { USHORT reg; int cur; const char *name; } words[] = {
        { 0x48c0, 0, "LAST_ADC_V" }, { 0x48c2, 1, "LAST_ADC_I" }, { 0x48c6, 1, "LAST_BURST_AVG_I" },
        { 0x4880, 0, "S2_NORMAL_AVG_V" }, { 0x4882, 1, "S2_NORMAL_AVG_I" }, { 0x4870, 0, "S7_PON_OCV_V" }, { 0x4874, 0, "S3_GOOD_OCV_V" },
        { 0x48cc, 0, "LAST_S3_SLEEP_V" },
    };
    ULONG v = 0, sub = 0;
    LONG s;
    unsigned i;
    int rc;

    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        rc = SpmiRead(core, 2, regs[i].reg, &v);
        if (rc == 0)
            Log("PMI632 %04x = %02lx  %s", regs[i].reg, (unsigned long)v, regs[i].name);
        else
            Log("PMI632 %04x: error %d", regs[i].reg, rc);
        if (regs[i].reg == 0x4805 && rc == 0)
            sub = v;
    }
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (SpmiRead16(core, words[i].reg, &v) != 0) {
            Log("PMI632 %04x: error", words[i].reg);
            continue;
        }
        if (!words[i].cur) {
            Log("PMI632 %04x = %04lx  %s = %ld mV", words[i].reg, (unsigned long)v, words[i].name, (long)((LONGLONG)v * 194637 / 1000000));
        } else {
            s = (LONG)(int16_t)(USHORT)v;
            Log("PMI632 %04x = %04lx  %s = %ld mA (+ discharge)", words[i].reg, (unsigned long)v, words[i].name,
                (long)((LONGLONG)s * (sub == 3 ? 152588 : 305176) / 1000000));
        }
    }
}

static void SpmiDump(void)
{
    static const struct { USHORT reg; const char *name; } regs[] = {
        { 0x1006, "BATTERY_CHARGER_STATUS_1 (bits 2:0 charge state)" }, { 0x1007, "BATTERY_CHARGER_STATUS_2" },
        { 0x1042, "CHARGING_ENABLE_CMD" }, { 0x1051, "FAST_CHARGE_CURRENT_CFG" }, { 0x1107, "ICL_STATUS" }, { 0x1108, "ADAPTER_ICL" },
        { 0x110a, "POWER_PATH_STATUS" }, { 0x110b, "AICL_STATUS" }, { 0x1306, "USBIN APSD/STATUS" }, { 0x1310, "USBIN INT_RT_STS" },
        { 0x1610, "MISC INT_RT_STS" }, { 0x1643, "BARK_BITE_WDOG_PET" }, { 0x1648, "MISC 0x48" }, { 0x1651, "WD_CFG" },
        { 0x1653, "SNARL_BARK_BITE_WD_CFG (bit7 bite disables charging)" }, { 0x1670, "MISC 0x70" },
    };
    volatile UCHAR *core = Map(SPMI_CORE_PA, 0x2000);
    ULONG ver, v = 0;
    unsigned i;
    int rc;

    if (!core)
        return;
    ver = Rd(core, 0);
    Log("SPMI arbiter version %08lx", (unsigned long)ver);
    if (ver < 0x50000000 || ver >= 0x70000000) {
        Log("SPMI: not v5, skip");
        goto out;
    }
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        rc = SpmiRead(core, 2, regs[i].reg, &v);
        if (rc == 0)
            Log("PMI632 %04x = %02lx  %s", regs[i].reg, (unsigned long)v, regs[i].name);
        else
            Log("PMI632 %04x: error %d", regs[i].reg, rc);
    }
    QgDump(core);
out:
    MmUnmapIoSpace((PVOID)core, 0x2000);
}

static void Probe(void)
{
    volatile UCHAR *vote, *qup, *gpio0, *gpio1, *wrap = NULL, *se = NULL;
    ULONG v, fw, depth, hwver;
    int on;

    vote = Map(GCC_VOTE_PA, 0x1000);
    qup = Map(GCC_QUP_PA, 0x1000);
    gpio0 = Map(TLMM_GPIO0_PA, 0x1000);
    gpio1 = Map(TLMM_GPIO0_PA + 0x1000, 0x1000);
    if (!vote || !qup || !gpio0 || !gpio1)
        goto out;

    Log("GCC vote 0x7900c = %08lx (want bits 6-10: %08lx)", (unsigned long)Rd(vote, VOTE_REG), (unsigned long)VOTE_BITS);
    Log("CBCR m_ahb %08lx s_ahb %08lx core %08lx core2x %08lx s0 %08lx", (unsigned long)Rd(qup, 0x004), (unsigned long)Rd(qup, 0x008),
        (unsigned long)Rd(qup, 0x00c), (unsigned long)Rd(qup, 0x014), (unsigned long)Rd(qup, 0x144));
    Log("SE0 RCGR cmd %08lx cfg %08lx m %08lx n %08lx d %08lx", (unsigned long)Rd(qup, 0x148), (unsigned long)Rd(qup, 0x14c),
        (unsigned long)Rd(qup, 0x150), (unsigned long)Rd(qup, 0x154), (unsigned long)Rd(qup, 0x158));
    Log("TLMM gpio0 cfg %08lx io %08lx; gpio1 cfg %08lx io %08lx (func = bits 5:2, qup0 = 1)", (unsigned long)Rd(gpio0, 0),
        (unsigned long)Rd(gpio0, 4), (unsigned long)Rd(gpio1, 0), (unsigned long)Rd(gpio1, 4));

    on = !(Rd(qup, 0x004) & CBCR_OFF) && !(Rd(qup, 0x008) & CBCR_OFF) && !(Rd(qup, 0x144) & CBCR_OFF);
    if (!on && !g_active) {
        Log("SE0 clocks are off; read-only mode stops here");
        goto out;
    }

    if (g_active) {
        int i;

        Log("active: vote QUP0 clocks");
        Wr(vote, VOTE_REG, Rd(vote, VOTE_REG) | VOTE_BITS);
        for (i = 0; i < 100; i++) {
            if (!(Rd(qup, 0x004) & CBCR_OFF) && !(Rd(qup, 0x008) & CBCR_OFF) && !(Rd(qup, 0x00c) & CBCR_OFF) &&
                !(Rd(qup, 0x144) & CBCR_OFF))
                break;
            SleepMs(1);
        }
        Log("CBCR m_ahb %08lx s_ahb %08lx core %08lx s0 %08lx after %d ms", (unsigned long)Rd(qup, 0x004), (unsigned long)Rd(qup, 0x008),
            (unsigned long)Rd(qup, 0x00c), (unsigned long)Rd(qup, 0x144), i);
        if (i == 100) {
            Log("clocks did not come up; stop");
            goto out;
        }
        // SE0 source: XO 19.2 MHz, no divider, no MND (cfg = 0) — the I2C timings below assume it
        v = Rd(qup, 0x14c);
        if (v & 0x371f) {
            Log("SE0 RCGR cfg %08lx -> XO", (unsigned long)v);
            Wr(qup, 0x14c, v & ~0x371fu);
            Wr(qup, 0x148, Rd(qup, 0x148) | 1);
            for (i = 0; i < 100 && (Rd(qup, 0x148) & 1); i++)
                SleepMs(1);
            Log("SE0 RCGR cmd %08lx cfg %08lx", (unsigned long)Rd(qup, 0x148), (unsigned long)Rd(qup, 0x14c));
        }
        // gpio0/1 → function qup0 (1), pull-up (3), keep the drive strength
        for (i = 0; i < 2; i++) {
            volatile UCHAR *g = i ? gpio1 : gpio0;
            v = Rd(g, 0);
            if (((v >> 2) & 0xf) != 1 || (v & 3) != 3) {
                Wr(g, 0, (v & ~0x3fu) | (1u << 2) | 3);
                Log("gpio%d cfg %08lx -> %08lx", i, (unsigned long)v, (unsigned long)Rd(g, 0));
            }
        }
    }

    wrap = Map(QUP0_WRAP_PA, 0x1000);
    se = Map(SE0_PA, SE_SIZE);
    if (!wrap || !se)
        goto out;
    DumpSe(se, wrap);
    if (!g_active)
        goto out;

    fw = Rd(se, GENI_FW_REVISION_RO);
    if (((fw >> 8) & 0xff) != 3) {
        Log("SE0 firmware protocol %lu, not I2C (3); stop", (unsigned long)((fw >> 8) & 0xff));
        goto out;
    }
    if (Rd(se, GENI_IF_DISABLE_RO) & 1) {
        Log("SE0 FIFO disabled (GPI DMA only); stop");
        goto out;
    }
    hwver = Rd(wrap, 0x4);
    v = Rd(se, SE_HW_PARAM_0);
    depth = ((hwver >> 28) > 3 || ((hwver >> 28) == 3 && ((hwver >> 16) & 0xfff) >= 10)) ? (v >> 16) & 0xff : (v >> 16) & 0x3f;
    Log("tx fifo depth %lu", (unsigned long)depth);
    if (depth < 2)
        depth = 16;

    // geni_se_init + FIFO mode + 8-bit packing (4 per word, msb_to_lsb) + qcom_geni_i2c_conf(100 kHz @ 19.2 MHz)
    Wr(se, SE_GSI_EVENT_EN, 0);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    Wr(se, SE_GENI_S_IRQ_CLEAR, 0xffffffff);
    Wr(se, SE_DMA_TX_IRQ_CLR, 0xffffffff);
    Wr(se, SE_DMA_RX_IRQ_CLR, 0xffffffff);
    Wr(se, SE_IRQ_EN, 0xffffffff);
    Wr(se, SE_GENI_CGC_CTRL, Rd(se, SE_GENI_CGC_CTRL) | 0x7f);
    Wr(se, SE_DMA_GENERAL_CFG, Rd(se, SE_DMA_GENERAL_CFG) | 0xf);
    Wr(se, GENI_OUTPUT_CTRL, 0x7f);
    Wr(se, GENI_FORCE_DEFAULT_REG, 1);
    Wr(se, SE_IRQ_EN, Rd(se, SE_IRQ_EN) | 0xf);
    Wr(se, SE_GENI_DMA_MODE_EN, Rd(se, SE_GENI_DMA_MODE_EN) & ~1u);
    Wr(se, SE_GSI_EVENT_EN, 0);
    Wr(se, SE_GENI_RX_WATERMARK, depth - 1);
    Wr(se, SE_GENI_RX_RFR_WM, depth);
    Wr(se, SE_GENI_M_IRQ_EN, Rd(se, SE_GENI_M_IRQ_EN) | M_COMMON_IRQ_EN | M_CMD_DONE | M_TX_FIFO_WM | M_RX_FIFO_WM | M_RX_FIFO_LAST);
    Wr(se, SE_GENI_TX_PACKING_CFG0, 0x7f8fe);
    Wr(se, SE_GENI_TX_PACKING_CFG1, 0xffefe);
    Wr(se, SE_GENI_RX_PACKING_CFG0, 0x7f8fe);
    Wr(se, SE_GENI_RX_PACKING_CFG1, 0xffefe);
    Wr(se, SE_GENI_BYTE_GRAN, 0);
    Wr(se, SE_GENI_CLK_SEL, 0);
    Wr(se, GENI_SER_M_CLK_CFG, (7u << 4) | 1);
    Wr(se, SE_I2C_SCL_COUNTERS, (10u << 20) | (12u << 10) | 26);
    Log("SE0 set up: status %08lx ios %08lx", (unsigned long)Rd(se, SE_GENI_STATUS), (unsigned long)Rd(se, SE_GENI_IOS));

    {
        static const struct { UCHAR reg; const char *name; } gauge[] = {
            { 0x06, "temperature 0.1K" }, { 0x08, "voltage mV" }, { 0x0a, "flags" }, { 0x0c, "nominal capacity mAh" },
            { 0x0e, "full charge capacity mAh" }, { 0x14, "average current mA (s16; Linux reports minus this)" }, { 0x16, "time to empty min" },
            { 0x18, "time to full min" }, { 0x2a, "cycle count" }, { 0x2c, "state of charge %" }, { 0x3c, "design capacity mAh" },
        };
        unsigned i;
        ULONG w;

        for (i = 0; i < sizeof(gauge) / sizeof(gauge[0]); i++) {
            if (ReadWord(se, gauge[i].reg, &w) == 0)
                Log("mm8013 %02x = 0x%04lx = %lu (%s)", gauge[i].reg, (unsigned long)w, (unsigned long)w, gauge[i].name);
            else if (i == 0) {
                Log("first gauge read failed; stop");
                break;
            }
        }
    }

out:
    if (se)
        MmUnmapIoSpace((PVOID)se, SE_SIZE);
    if (wrap)
        MmUnmapIoSpace((PVOID)wrap, 0x1000);
    if (gpio1)
        MmUnmapIoSpace((PVOID)gpio1, 0x1000);
    if (gpio0)
        MmUnmapIoSpace((PVOID)gpio0, 0x1000);
    if (qup)
        MmUnmapIoSpace((PVOID)qup, 0x1000);
    if (vote)
        MmUnmapIoSpace((PVOID)vote, 0x1000);
}

static VOID ProbeUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

// Service name = last component of the registry path; "...i2c" selects the active mode.
static int ServiceIsI2c(PUNICODE_STRING path)
{
    USHORT n = path->Length / sizeof(WCHAR);
    PWCH s = path->Buffer;

    return n >= 3 && (s[n - 3] | 0x20) == 'i' && s[n - 2] == '2' && (s[n - 1] | 0x20) == 'c';
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DriverObject->DriverUnload = ProbeUnload;
    g_active = RegistryPath && RegistryPath->Buffer && ServiceIsI2c(RegistryPath);
    Log("=== j606f probe start, mode %s", g_active ? "ACTIVE (i2c0 + mm8013)" : "read-only");
    SpmiDump();
    Probe();
    Log("=== j606f probe end");
    return STATUS_SUCCESS;
}
