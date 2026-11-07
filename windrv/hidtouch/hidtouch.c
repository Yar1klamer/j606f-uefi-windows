// J606F touch as a real HID touch screen (Windows ARM64 kernel driver, test-signed): a WDM HID minidriver (HidRegisterMinidriver) for the
// ACPI device JHMX0001 (\_SB.TSC0 in the j606f SSDT, UEFI v6). On START a system thread brings up QUP0 SE2 as a GENI SPI master
// (FIFO mode, CPHA 1 + CPOL 1, 10 MHz), uploads the zero-flash firmware of the Himax HX83102E (C:\j606f\drv\hxfw.bin, = Himax_firmware.bin)
// and then polls the event stack (bus read 0x30 x 128) when gpio86 latched a falling edge (TLMM raw status, the TLMM interrupt itself
// stays disabled) or is low. Frames are turned into multi-touch digitizer reports (Touch Screen, 10 contacts, parallel mode) and handed
// to hidclass through pended IOCTL_HID_READ_REPORT IRPs. Hardware code = windrv/touch/touch.c step 2b (Windows test run_18, 2026-11-04).
// Event frame (himax_common.c, HX_MAX_PT 10, pen on): 0..39 points (x hi, x lo, y hi, y lo; FFFF = none), 40..51 area, 52 point count
// (FF = none), 56..67 pen; byte sum of 0..67 is 0 mod 256. Raw coordinates are chassis coordinates 1200 x 2000; the panel is mounted at
// 180 degrees (pmOS notes/touch.md), so by default both axes are inverted. Registry DWORD TouchMap in the service key overrides:
// bit0 invert X, bit1 invert Y, bit2 swap X/Y (read at every START). Log: C:\j606f\out\hidtouch.txt.
#include "j606f_wdm.h"
#define GCC_VOTE_PA     0x01479000
#define GCC_QUP_PA      0x0141f000
#define TLMM_WEST_PA    0x00500000
#define TLMM_EAST_PA    0x00d00000
#define SE2_PA          0x04a88000
#define SE_SIZE         0x4000

#define VOTE_REG        0x00c
#define VOTE_BITS       ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 12))  // m_ahb, s_ahb, core, core_2x, s2
#define CBCR_OFF        (1u << 31)
#define S2_CBCR         0x3a4
#define S2_RCGR_CMD     0x3a8
#define S2_RCGR_CFG     0x3ac

#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define SE_GENI_CGC_CTRL        0x28
#define SE_GENI_STATUS          0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_IF_DISABLE_RO      0x64
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_SPI_CPHA             0x224
#define SE_SPI_LOOPBACK         0x22c
#define SE_SPI_CPOL             0x230
#define SE_SPI_DEMUX_OUTPUT_INV 0x24c
#define SE_SPI_DEMUX_SEL        0x250
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_SPI_TRANS_CFG        0x25c
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_SPI_WORD_LEN         0x268
#define SE_SPI_TX_TRANS_LEN     0x26c
#define SE_SPI_RX_TRANS_LEN     0x270
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_EN        0x644
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK    0x80c
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WM       0x814
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
#define M_RX_FIFO_WM      (1u << 26)
#define M_RX_FIFO_LAST    (1u << 27)
#define M_TX_FIFO_WM      (1u << 30)
#define M_COMMON_IRQ_EN   (0x7eu | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_TX_RX         7u

static void LogRaw(const char *text)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    ULONG len = 0;

    while (text[len])
        len++;
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\hidtouch.txt");
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
    DbgPrint("j606f hidtouch: %s", line);
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

static volatile UCHAR *g_se;
static ULONG g_fifo_depth = 16;

#define SPI_TX_ONLY       1u
#define SPI_RX_ONLY       2u
#define FRAGMENTATION     (1u << 2)

// One GENI SPI command (spi-geni-qcom setup_se_xfer + FIFO polling): opcode TX_ONLY / RX_ONLY / TX_RX, len bytes.
// FRAGMENTATION keeps CS asserted after the command (next transfer of the same message follows). 0 = ok.
static int SpiCmd(ULONG opcode, ULONG params, const UCHAR *tx, UCHAR *rx, ULONG len)
{
    volatile UCHAR *se = g_se;
    ULONG words = (len + 3) / 4, txw = 0, rxw = 0, st, i, n, w;
    ULONG need_tx = (opcode != SPI_RX_ONLY), need_rx = (opcode != SPI_TX_ONLY);
    int spin;

    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    if (need_tx)
        Wr(se, SE_SPI_TX_TRANS_LEN, len);
    if (need_rx)
        Wr(se, SE_SPI_RX_TRANS_LEN, len);
    Wr(se, SE_GENI_M_CMD0, (opcode << 27) | params);
    for (spin = 0; spin < 200000; spin++) {
        while (need_tx && txw < words && (Rd(se, SE_GENI_TX_FIFO_STATUS) & 0x0fffffff) < g_fifo_depth) {
            w = 0;
            for (i = 0; i < 4 && txw * 4 + i < len; i++)
                w |= (ULONG)(tx ? tx[txw * 4 + i] : 0) << (8 * i);
            Wr(se, SE_GENI_TX_FIFOn, w);
            txw++;
        }
        if (need_rx) {
            n = Rd(se, SE_GENI_RX_FIFO_STATUS) & 0x01ffffff;
            while (n-- && rxw < words) {
                w = Rd(se, SE_GENI_RX_FIFOn);
                for (i = 0; i < 4 && rxw * 4 + i < len; i++)
                    if (rx)
                        rx[rxw * 4 + i] = (UCHAR)(w >> (8 * i));
                rxw++;
            }
        }
        st = Rd(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_FAILURE | M_CMD_ABORT)) {
            Log("spi op %lu: M_IRQ %08lx (failure/abort), tx %lu rx %lu of %lu words", (unsigned long)opcode, (unsigned long)st,
                (unsigned long)txw, (unsigned long)rxw, (unsigned long)words);
            Wr(se, SE_GENI_M_IRQ_CLEAR, st);
            return -1;
        }
        if ((st & M_CMD_DONE) && (!need_rx || rxw >= words)) {
            Wr(se, SE_GENI_M_IRQ_CLEAR, st);
            return 0;
        }
        if (spin > 1000)
            KeStallExecutionProcessor(5);
    }
    Log("spi op %lu: timeout, M_IRQ %08lx STATUS %08lx TXFS %08lx RXFS %08lx, tx %lu rx %lu of %lu words", (unsigned long)opcode,
        (unsigned long)Rd(se, SE_GENI_M_IRQ_STATUS), (unsigned long)Rd(se, SE_GENI_STATUS), (unsigned long)Rd(se, SE_GENI_TX_FIFO_STATUS),
        (unsigned long)Rd(se, SE_GENI_RX_FIFO_STATUS), (unsigned long)txw, (unsigned long)rxw, (unsigned long)words);
    Wr(se, SE_GENI_M_CMD_CTRL_REG, 1u << 2);    // M_GENI_CMD_CANCEL
    SleepMs(2);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    return -2;
}

// g_split = 1: like Linux (himax_spi_read = 2 transfers in one message): TX_ONLY header with FRAGMENTATION, then RX_ONLY data;
// writes are TX_ONLY. g_split = 0: one full-duplex TX_RX command (steps 1b/1c).
static int g_split = 1;
static int g_rx_cpha = -1;          // >= 0: CPHA used only for the RX_ONLY data phase of a split read
static ULONG g_tx_cpha_saved;

static int SpiXfer(const UCHAR *tx, UCHAR *rx, ULONG len)
{
    return SpiCmd(SPI_TX_RX, 0, tx, rx, len);
}

static int SpiWrite(const UCHAR *tx, ULONG len)
{
    return g_split ? SpiCmd(SPI_TX_ONLY, 0, tx, NULL, len) : SpiXfer(tx, NULL, len);
}

// Read styles (split = Linux-like): 0 = TX_ONLY "F3 cmd 00" + FRAGMENTATION, RX_ONLY n (as himax_spi_read);
// 1 = TX_ONLY "F3 cmd" + FRAGMENTATION, TX_RX "00" + n zeros; 2 = TX_ONLY "F3 cmd" + FRAGMENTATION, RX_ONLY 1 + n.
// out gets 3 + n bytes as on the wire; for styles 1/2 out[2] is what was received during the dummy byte.
static int g_rdstyle = 0;

static int SpiReadMsg(const UCHAR *hdr, UCHAR *out, ULONG n)
{
    UCHAR tx[24] = { 0 };
    ULONG i;
    int rc;

    if (!g_split) {
        for (i = 0; i < 3; i++)
            tx[i] = hdr[i];
        return SpiXfer(tx, out, 3 + n);
    }
    for (i = 0; i < 3; i++)
        out[i] = 0;
    rc = SpiCmd(SPI_TX_ONLY, FRAGMENTATION, hdr, NULL, g_rdstyle ? 2 : 3);
    if (rc)
        return rc;
    if (g_rdstyle == 1)
        return SpiCmd(SPI_TX_RX, 0, tx, out + 2, 1 + n);
    if (g_rdstyle == 2)
        return SpiCmd(SPI_RX_ONLY, 0, NULL, out + 2, 1 + n);
    return SpiCmd(SPI_RX_ONLY, 0, NULL, out + 3, n);
}

// run_14/15: with CPHA 0 the IC's data arrive one bit early (rx = data << 1, 0x13 = 0x63 for 0x31, safe mode 0x18 for 0x0C).
// With the dummy byte received (styles 1/2) the lost first bit is the dummy's LSB: data[i] = rx[i] >> 1 | rx[i - 1] << 7.
static int g_fixshift = 0;

static void FixShift(const UCHAR *wire, UCHAR *data, ULONG n)
{
    ULONG i;

    for (i = 0; i < n; i++)
        data[i] = g_fixshift ? (UCHAR)((wire[3 + i] >> 1) | (wire[2 + i] << 7)) : wire[3 + i];
}

static int HxWrite(UCHAR cmd, const UCHAR *data, ULONG n)
{
    UCHAR buf[16];
    ULONG i;

    if (n > sizeof(buf) - 2)
        return -3;
    buf[0] = 0xF2;
    buf[1] = cmd;
    for (i = 0; i < n; i++)
        buf[2 + i] = data[i];
    return SpiWrite(buf, n + 2);
}

static int HxRead(UCHAR cmd, UCHAR *data, ULONG n)
{
    UCHAR tx[3] = { 0xF3, 0, 0 }, rx[140] = { 0 };
    int rc;

    if (n > sizeof(rx) - 3)
        return -3;
    tx[1] = cmd;
    rc = SpiReadMsg(tx, rx, n);
    FixShift(rx, data, n);
    return rc;
}

static int HxRegRead(ULONG addr, UCHAR *out4)
{
    UCHAR b[4];
    int rc;

    b[0] = 0x31;
    rc = HxWrite(0x13, b, 1);           // burst: conti
    b[0] = 0x10;
    if (!rc) rc = HxWrite(0x0D, b, 1);  // incr4, no auto +4
    b[0] = (UCHAR)addr; b[1] = (UCHAR)(addr >> 8); b[2] = (UCHAR)(addr >> 16); b[3] = (UCHAR)(addr >> 24);
    if (!rc) rc = HxWrite(0x00, b, 4);  // AHB address
    b[0] = 0x00;
    if (!rc) rc = HxWrite(0x0C, b, 1);  // direction: read
    if (!rc) rc = HxRead(0x08, out4, 4);
    return rc;
}

static void SetMode(ULONG cpha, ULONG cpol, ULONG div)
{
    Wr(g_se, SE_SPI_CPHA, cpha);
    Wr(g_se, SE_SPI_CPOL, cpol ? (1u << 2) : 0);
    Wr(g_se, GENI_SER_M_CLK_CFG, (div << 4) | 1);
}

// Register read like HxRegRead, but the data phase reads 8 bytes and the raw RX of the whole F3 08 00 + 8 transfer is logged,
// so an answer shifted by bits or bytes is visible.
static int HxRegReadRaw(ULONG addr, UCHAR *out4, const char *tag)
{
    UCHAR b[4], tx[11] = { 0xF3, 0x08, 0x00 }, rx[11] = { 0 };
    ULONG i;
    int rc;

    b[0] = 0x31;
    rc = HxWrite(0x13, b, 1);
    b[0] = 0x10;
    if (!rc) rc = HxWrite(0x0D, b, 1);
    b[0] = (UCHAR)addr; b[1] = (UCHAR)(addr >> 8); b[2] = (UCHAR)(addr >> 16); b[3] = (UCHAR)(addr >> 24);
    if (!rc) rc = HxWrite(0x00, b, 4);
    b[0] = 0x00;
    if (!rc) rc = HxWrite(0x0C, b, 1);
    if (!rc) rc = SpiReadMsg(tx, rx, 8);
    FixShift(rx, out4, 4);
    (void)i;
    Log("%s: reg %08lx rc %d raw %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x -> %02x %02x %02x %02x", tag, (unsigned long)addr, rc,
        rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7], rx[8], rx[9], rx[10], out4[0], out4[1], out4[2], out4[3]);
    return rc;
}

// hx83102_flash_write_burst after burst_enable(0): cmd 00 with address LE + data LE
static int HxRegWrite(ULONG addr, ULONG val)
{
    UCHAR b[8];
    int rc;

    b[0] = 0x31;
    rc = HxWrite(0x13, b, 1);
    b[0] = 0x10;
    if (!rc) rc = HxWrite(0x0D, b, 1);
    b[0] = (UCHAR)addr; b[1] = (UCHAR)(addr >> 8); b[2] = (UCHAR)(addr >> 16); b[3] = (UCHAR)(addr >> 24);
    b[4] = (UCHAR)val; b[5] = (UCHAR)(val >> 8); b[6] = (UCHAR)(val >> 16); b[7] = (UCHAR)(val >> 24);
    if (!rc) rc = HxWrite(0x00, b, 8);
    return rc;
}

static volatile UCHAR *g_rst;

// hx83102_pin_reset / the retry inside hx83102e_sense_off: low, then high and wait
static void PinReset(LONG low_ms, LONG high_ms)
{
    Wr(g_rst, 4, 0);
    Wr(g_rst, 0, (1u << 9) | (3u << 6) | 3u);
    SleepMs(low_ms);
    Wr(g_rst, 4, 2);
    SleepMs(high_ms);
}

// hx83102e_sense_off(check_en = false) as the zero-flash probe calls it: FW stop, I2C password (safe mode), check 0x0C, TCON reset
static int SenseOff(void)
{
    UCHAR d[4], b;
    int cnt;

    HxRegWrite(0x9000005C, 0xA5);       // addr_ctrl_fw_isr <- data_fw_stop
    SleepMs(10);
    HxRegReadRaw(0x900000A8, d, "sense_off central state");
    for (cnt = 0; cnt < 5; cnt++) {
        b = 0x27;
        HxWrite(0x31, &b, 1);
        b = 0x95;
        HxWrite(0x32, &b, 1);
        HxRegReadRaw(0x900000A8, d, "sense_off safe mode");
        if (d[0] == 0x0C) {
            HxRegWrite(0x80020020, 0);  // TCON reset
            SleepMs(1);
            return 1;
        }
        SleepMs(5);
        PinReset(20, 50);
    }
    return 0;
}


static volatile UCHAR *g_qup;

// QUP0 SE2 core clock: fast = 100 MHz (GPLL0_OUT_AUX2 300 MHz / 3, as pmOS: clk_summary 100000000, 10 MHz SPI = div 10),
// otherwise XO 19.2 MHz. rcg2: CFG bits 4:0 = 2*div-1, 10:8 = source (gcc_parent_map_1: 0 XO, 2 GPLL0_OUT_AUX2); CMD bit 0 UPDATE, bit 31 ROOT_OFF.
static int SetSeClk(int fast)
{
    ULONG v = Rd(g_qup, S2_RCGR_CFG), c;
    int i;

    Wr(g_qup, S2_RCGR_CFG, (v & ~0x371fu) | (fast ? ((2u << 8) | 5u) : 0));
    Wr(g_qup, S2_RCGR_CMD, Rd(g_qup, S2_RCGR_CMD) | 1);
    for (i = 0; i < 100 && (Rd(g_qup, S2_RCGR_CMD) & 1); i++)
        SleepMs(1);
    c = Rd(g_qup, S2_RCGR_CMD);
    Log("SE2 clock -> %s: RCGR cfg %08lx -> %08lx, cmd %08lx (%d ms), CBCR %08lx", fast ? "100 MHz (GPLL0_AUX2/3)" : "XO 19.2 MHz",
        (unsigned long)v, (unsigned long)Rd(g_qup, S2_RCGR_CFG), (unsigned long)c, i, (unsigned long)Rd(g_qup, S2_CBCR));
    return !(c & 1) && !(c & 0x80000000u);
}


// ---- step 2: zero-flash firmware upload (himax_mcu_firmware_update_0f + himax_zf_part_info, HX_CODE_OVERLAY, G_POWERONOF = 1) ----

// TX_ONLY of hdr followed by data, streamed into the FIFO (for writes up to 64 KiB + header)
static int SpiTx2(const UCHAR *hdr, ULONG hlen, const UCHAR *data, ULONG dlen)
{
    volatile UCHAR *se = g_se;
    ULONG len = hlen + dlen, words = (len + 3) / 4, txw = 0, st, i, k, w;
    int spin;

    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    Wr(se, SE_SPI_TX_TRANS_LEN, len);
    Wr(se, SE_GENI_M_CMD0, SPI_TX_ONLY << 27);
    for (spin = 0; spin < 2000000; spin++) {
        while (txw < words && (Rd(se, SE_GENI_TX_FIFO_STATUS) & 0x0fffffff) < g_fifo_depth) {
            w = 0;
            for (i = 0; i < 4; i++) {
                k = txw * 4 + i;
                if (k < len)
                    w |= (ULONG)(k < hlen ? hdr[k] : data[k - hlen]) << (8 * i);
            }
            Wr(se, SE_GENI_TX_FIFOn, w);
            txw++;
        }
        st = Rd(se, SE_GENI_M_IRQ_STATUS);
        if (st & (M_CMD_FAILURE | M_CMD_ABORT)) {
            Log("spi tx2: M_IRQ %08lx, tx %lu of %lu words", (unsigned long)st, (unsigned long)txw, (unsigned long)words);
            Wr(se, SE_GENI_M_IRQ_CLEAR, st);
            return -1;
        }
        if (st & M_CMD_DONE) {
            Wr(se, SE_GENI_M_IRQ_CLEAR, st);
            return 0;
        }
        if (txw >= words)
            KeStallExecutionProcessor(2);
    }
    Log("spi tx2: timeout, tx %lu of %lu words", (unsigned long)txw, (unsigned long)words);
    Wr(se, SE_GENI_M_CMD_CTRL_REG, 1u << 2);
    SleepMs(2);
    Wr(se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    return -2;
}

static void Burst(int incr)
{
    UCHAR b = 0x31;
    HxWrite(0x13, &b, 1);
    b = (UCHAR)(0x10 | (incr ? 1 : 0));
    HxWrite(0x0D, &b, 1);
}

// himax_mcu_register_write: burst (auto +4 if > 4 bytes), cmd 00 with address + data, chunks of HX_MAX_WRITE_SZ - 4 = 64 KiB
static int RegWriteBuf(ULONG addr, const UCHAR *data, ULONG len)
{
    UCHAR hdr[6];
    ULONG off = 0, n, a;
    int rc = 0;

    Burst(len > 4);
    while (off < len && !rc) {
        n = len - off > 65536 ? 65536 : len - off;
        a = addr + off;
        hdr[0] = 0xF2; hdr[1] = 0x00;
        hdr[2] = (UCHAR)a; hdr[3] = (UCHAR)(a >> 8); hdr[4] = (UCHAR)(a >> 16); hdr[5] = (UCHAR)(a >> 24);
        rc = SpiTx2(hdr, 6, data + off, n);
        off += n;
        KeStallExecutionProcessor(100);
    }
    return rc;
}

// himax_mcu_register_read for n <= 128 bytes (burst with auto +4 when n > 4)
static int RegReadN(ULONG addr, UCHAR *out, ULONG n)
{
    UCHAR b[4];
    int rc;

    Burst(n > 4);
    b[0] = (UCHAR)addr; b[1] = (UCHAR)(addr >> 8); b[2] = (UCHAR)(addr >> 16); b[3] = (UCHAR)(addr >> 24);
    rc = HxWrite(0x00, b, 4);
    b[0] = 0x00;
    if (!rc) rc = HxWrite(0x0C, b, 1);
    if (!rc) rc = HxRead(0x08, out, n);
    return rc;
}

static ULONG Le32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static ULONG RegRd32(ULONG addr)
{
    UCHAR d[4] = { 0 };
    RegReadN(addr, d, 4);
    return Le32(d);
}

// himax_mcu_Calculate_CRC_with_AP
static ULONG SwCrc(const UCHAR *p, ULONG len)
{
    ULONG crc = 0xFFFFFFFF, i, j;

    for (i = 0; i < len / 4; i++) {
        crc ^= Le32(p + 4 * i);
        for (j = 0; j < 32; j++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78) : (crc >> 1);
    }
    return crc;
}

// himax_mcu_check_CRC: HW CRC32 of len bytes of SRAM at addr (0x80050020 from, 0x80050028 0x0099<<16 | words, 0x80050000 bit0 busy)
static ULONG HwCrc(ULONG addr, ULONG len)
{
    int cnt;

    HxRegWrite(0x80050020, addr);
    HxRegWrite(0x80050028, 0x00990000u | ((len / 4) & 0xffff));
    for (cnt = 0; cnt < 100; cnt++) {
        if (!(RegRd32(0x80050000) & 1))
            return RegRd32(0x80050018);
        SleepMs(1);
    }
    Log("HW CRC: busy timeout");
    return 0xEEEEEEEE;
}

// himax_sram_write_crc_check: part carries its own CRC, so HW CRC over it must be 0
static int SramWriteCrc(ULONG sram, const UCHAR *data, ULONG len, const char *what)
{
    ULONG crc = 1;
    int retry;

    for (retry = 0; retry < 3; retry++) {
        RegWriteBuf(sram, data, len);
        crc = HwCrc(sram, len);
        Log("  %s: %lu bytes -> %08lx, HW CRC %08lx (%s)", what, (unsigned long)len, (unsigned long)sram, (unsigned long)crc,
            crc == 0 ? "OK" : "FAIL");
        if (crc == 0)
            return 0;
    }
    return -1;
}

// himax_mcu_interface_on
static void InterfaceOn(void)
{
    UCHAR d[4], c1, c2;
    int cnt;

    HxRead(0x08, d, 4);
    for (cnt = 0; cnt < 10; cnt++) {
        Burst(0);
        HxRead(0x13, &c1, 1);
        HxRead(0x0D, &c2, 1);
        if (c1 == 0x31 && c2 == 0x10)
            break;
        SleepMs(1);
    }
}

static int HandshakeWrite(ULONG v)
{
    int retry;
    ULONG r = 0;

    for (retry = 0; retry < 10; retry++) {
        HxRegWrite(0x10007FFC, v);
        KeStallExecutionProcessor(1000);
        r = RegRd32(0x10007FFC);
        if (r == v)
            return 0;
    }
    Log("  handshake write %08lx: read back %08lx", (unsigned long)v, (unsigned long)r);
    return -1;
}

// hx83102e_reload_to_active: 0x90000048 <- EC until it reads back xx xx 01 EC
static void ReloadToActive(void)
{
    ULONG r = 0;
    int n;

    for (n = 0; n < 10; n++) {
        HxRegWrite(0x90000048, 0xEC);
        SleepMs(1);
        r = RegRd32(0x90000048);
        if ((r & 0xffff) == 0x01EC)
            break;
    }
    Log("  reload_to_active: %08lx after %d tries", (unsigned long)r, n + 1);
}

static UCHAR *LoadFile(const WCHAR *name, ULONG *size)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES attrs;
    IO_STATUS_BLOCK iosb;
    HANDLE file;
    UCHAR *buf;
    NTSTATUS st;

    *size = 0;
    RtlInitUnicodeString(&path, name);
    InitializeObjectAttributes(&attrs, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&file, 0x80100000u /* GENERIC_READ | SYNCHRONIZE */, &attrs, &iosb, NULL, 0, FILE_SHARE_READ, 1 /* FILE_OPEN */,
                      0x20 | 0x40 /* SYNCHRONOUS_IO_NONALERT | NON_DIRECTORY_FILE */, NULL, 0);
    if (st < 0) {
        Log("open firmware: status %08lx", (unsigned long)st);
        return NULL;
    }
    buf = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 0x40000, 0x66363036 /* '606f' */);
    if (buf) {
        st = ZwReadFile(file, NULL, NULL, NULL, &iosb, buf, 0x40000, NULL, NULL);
        if (st >= 0)
            *size = (ULONG)iosb.Information;
        else
            Log("read firmware: status %08lx", (unsigned long)st);
    }
    ZwClose(file);
    return buf;
}

#define CFG_TABLE 0x10400           // hx83102e_chip_init: CFG_TABLE_FLASH_ADDR

static int ZfUpload(const UCHAR *fw, ULONG size)
{
    ULONG nparts, i, sram[8], wsz[8], fwa[8], base = 0xFFFFFFFF, max = 0, imin = 0, imax = 0, cfgsz, swcrc, hwcrc = 0, r = 0, v;
    int ovl = -1, retry;
    UCHAR *cfg;

    if (size < CFG_TABLE + 16 * 8)
        return -1;
    nparts = fw[CFG_TABLE + 12];
    if (nparts < 2 || nparts > 8) {
        Log("zf: bad part count %lu", (unsigned long)nparts);
        return -1;
    }
    for (i = 0; i < nparts; i++) {
        const UCHAR *b = fw + CFG_TABLE + 16 * i;
        sram[i] = Le32(b);
        wsz[i] = Le32(b + 4);
        fwa[i] = Le32(b + 8);
        if (fwa[i] + wsz[i] > size) {
            Log("zf: part %lu outside the file", (unsigned long)i);
            return -1;
        }
        if (i == 0)
            continue;
        if (b[15] == 0x77 && b[14] == 0x88) {
            ovl = (int)i;
            continue;
        }
        if (base > sram[i]) { base = sram[i]; imin = i; }
        if (max < sram[i]) { max = sram[i]; imax = i; }
    }
    cfgsz = (max - base) + wsz[imax];
    if (cfgsz % 16)
        cfgsz += 16 - cfgsz % 16;
    Log("zf: %lu parts, main %08lx/%lu, cfg %08lx..%08lx size %lu, overlay %d", (unsigned long)nparts, (unsigned long)sram[0],
        (unsigned long)wsz[0], (unsigned long)base, (unsigned long)max, (unsigned long)cfgsz, ovl);
    if (cfgsz > 32768 || ovl < 0)
        return -1;
    cfg = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, cfgsz, 0x66363036);
    if (!cfg)
        return -1;
    for (i = 0; i < cfgsz; i++)
        cfg[i] = 0;
    for (i = 1; i < nparts; i++) {
        ULONG a = sram[i] & ~3u, k;
        if ((int)i == ovl)
            continue;
        for (k = 0; k < wsz[i]; k++)
            cfg[a - base + k] = fw[fwa[i] + k];
    }
    swcrc = SwCrc(cfg, cfgsz);
    (void)imin;

    // himax_mcu_firmware_update_0f: system reset, sense off
    HxRegWrite(0x90000018, 0x55);
    Log("zf: sense_off %s", SenseOff() ? "ok" : "FAILED");

    if (SramWriteCrc(sram[0], fw + fwa[0], wsz[0], "main FW")) {
        Log("zf: main FW CRC failed");
        ExFreePoolWithTag(cfg, 0x66363036);
        return -2;
    }
    for (retry = 0; retry < 3; retry++) {
        RegWriteBuf(base, cfg, cfgsz);
        hwcrc = HwCrc(base, cfgsz);
        Log("  config: %lu bytes -> %08lx, HW CRC %08lx SW CRC %08lx (%s)", (unsigned long)cfgsz, (unsigned long)base, (unsigned long)hwcrc,
            (unsigned long)swcrc, hwcrc == swcrc ? "OK" : "FAIL");
        if (hwcrc == swcrc)
            break;
    }
    ExFreePoolWithTag(cfg, 0x66363036);

    // overlay: clear handshake, start FW, wait for its request, load the overlay, reply
    HandshakeWrite(0xA55A5AA5);
    HxRegWrite(0x10007F00, 0);              // reload_disable(0): flash reload enable
    HxRegWrite(0x100072EC, 0);              // raw_out_sel <- clear (hx83102e_fw_addr_raw_out_sel)
    HxRegWrite(0x10007F04, 0);              // sorting mode <- clear
    HxRegWrite(0x100072C0, 0);              // 2nd flash reload flag <- 0
    // hx83102e_sense_on(0): interface on, FW isr clear, reset pin (zero flash: 5 ms low, 5 ms high), reload_to_active
    InterfaceOn();
    HxRegWrite(0x9000005C, 0);
    SleepMs(10);
    PinReset(5, 5);
    ReloadToActive();
    for (retry = 0; retry <= 300; retry++) {
        r = RegRd32(0x10007FFC);
        if (r == 0x11111111)
            break;
        SleepMs(1);
    }
    Log("zf: overlay request: %08lx after %d polls", (unsigned long)r, retry);
    v = 0x22222222;
    if (r != 0x11111111 || SramWriteCrc(sram[ovl], fw + fwa[ovl], wsz[ovl], "overlay"))
        v = 0xFF;
    HandshakeWrite(v);
    for (retry = 0; retry < 30; retry++) {
        r = RegRd32(0x100072C0);
        if (r == 0x000072C0)
            break;
        SleepMs(10);
    }
    Log("zf: reload done flag %08lx after %d polls (%s)", (unsigned long)r, retry, r == 0x72C0 ? "FW running" : "not done");
    return r == 0x72C0 ? 0 : -3;
}

// =====================================================================================================================================
// hardware bring-up (Run() of touch step 2b without the experiments)

static volatile UCHAR *g_vote, *g_irq, *g_pin[4];
static ULONG g_map = 3;             // TouchMap: bit0 invert X, bit1 invert Y, bit2 swap X/Y
static WCHAR g_regbuf[260];
static UNICODE_STRING g_regpath;

#define TLMM_INTR_CFG    0x8
#define TLMM_INTR_STATUS 0xc

static int HwInit(void)
{
    static const ULONG pins[4] = { 6, 7, 71, 80 };
    volatile UCHAR *se, *qup;
    ULONG v, fw, p0, size = 0;
    UCHAR id[4] = { 0 }, b1 = 0, *img;
    int i, rc;

    if (!g_vote) {
        g_vote = Map(GCC_VOTE_PA, 0x1000);
        g_qup = Map(GCC_QUP_PA, 0x1000);
        for (i = 0; i < 4; i++)
            g_pin[i] = Map(TLMM_WEST_PA + 0x1000 * pins[i], 0x1000);
        g_irq = Map(TLMM_WEST_PA + 0x1000 * 86, 0x1000);
        g_rst = Map(TLMM_EAST_PA + 0x1000 * 54, 0x1000);
        g_se = Map(SE2_PA, SE_SIZE);
    }
    if (!g_vote || !g_qup || !g_pin[0] || !g_pin[1] || !g_pin[2] || !g_pin[3] || !g_irq || !g_rst || !g_se)
        return -1;
    se = g_se;
    qup = g_qup;

    Wr(g_vote, VOTE_REG, Rd(g_vote, VOTE_REG) | VOTE_BITS);
    for (i = 0; i < 100; i++) {
        if (!(Rd(qup, 0x004) & CBCR_OFF) && !(Rd(qup, 0x008) & CBCR_OFF) && !(Rd(qup, S2_CBCR) & CBCR_OFF))
            break;
        SleepMs(1);
    }
    if (i == 100) {
        Log("S2 clocks did not come up");
        return -1;
    }
    for (i = 0; i < 4; i++)             // SPI pins: function 1 (qup2), no pull, 6 mA
        Wr(g_pin[i], 0, (1u << 2) | (2u << 6));
    Wr(g_irq, 0, 3u | (3u << 6));       // gpio86: GPIO input, pull-up
    fw = Rd(se, GENI_FW_REVISION_RO);
    p0 = Rd(se, SE_HW_PARAM_0);
    if (((fw >> 8) & 0xff) != 1 || (Rd(se, GENI_IF_DISABLE_RO) & 1)) {
        Log("SE2 not usable as FIFO SPI: FW_REVISION %08lx IF_DISABLE %08lx", (unsigned long)fw, (unsigned long)Rd(se, GENI_IF_DISABLE_RO));
        return -1;
    }
    v = (p0 >> 16) & 0x3f;
    if (v >= 4)
        g_fifo_depth = v;

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
    Wr(se, SE_GENI_RX_WATERMARK, 0);
    Wr(se, SE_GENI_RX_RFR_WM, g_fifo_depth - 2);
    Wr(se, SE_GENI_M_IRQ_EN, Rd(se, SE_GENI_M_IRQ_EN) | M_COMMON_IRQ_EN | M_CMD_DONE | M_TX_FIFO_WM | M_RX_FIFO_WM | M_RX_FIFO_LAST);
    Wr(se, SE_SPI_LOOPBACK, 0);
    Wr(se, SE_SPI_DEMUX_SEL, 0);
    Wr(se, SE_SPI_DEMUX_OUTPUT_INV, 0);
    Wr(se, SE_SPI_TRANS_CFG, Rd(se, SE_SPI_TRANS_CFG) & ~(1u << 1));
    Wr(se, SE_GENI_TX_PACKING_CFG0, 0x7f8fe);
    Wr(se, SE_GENI_TX_PACKING_CFG1, 0xffefe);
    Wr(se, SE_GENI_RX_PACKING_CFG0, 0x7f8fe);
    Wr(se, SE_GENI_RX_PACKING_CFG1, 0xffefe);
    Wr(se, SE_GENI_BYTE_GRAN, 0);
    Wr(se, SE_SPI_WORD_LEN, 8 - 4);
    Wr(se, SE_GENI_CLK_SEL, 0);
    g_split = 1;
    g_rdstyle = 0;
    g_fixshift = 0;
    if (!SetSeClk(1)) {
        Log("SE2 100 MHz failed");
        return -1;
    }
    SetMode(1, 1, 10);                  // run_16: CPOL 1 required; CPHA 1, 10 MHz

    PinReset(5, 50);
    rc = HxRead(0x13, &b1, 1);
    Log("bus read 0x13: rc %d, %02x; sense_off %s", rc, b1, SenseOff() ? "ok" : "FAILED");
    HxRegReadRaw(0x900000D0, id, "IC ID");
    if (!(id[3] == 0x83 && id[2] == 0x10 && id[1] == 0x2E)) {
        Log("not HX83102E");
        return -1;
    }
    img = LoadFile(L"\\??\\C:\\j606f\\drv\\hxfw.bin", &size);
    if (!img || size != 131072) {
        Log("firmware missing or wrong size (%lu)", (unsigned long)size);
        if (img)
            ExFreePoolWithTag(img, 0x66363036);
        return -1;
    }
    rc = ZfUpload(img, size);
    ExFreePoolWithTag(img, 0x66363036);
    {
        UCHAR d[8] = { 0 };
        RegReadN(0x10007004, d, 4);
        Log("zf upload %d; FW ver %02x %02x %02x %02x", rc, d[0], d[1], d[2], d[3]);
    }
    if (rc)
        return -1;
    // gpio86 edge latch: falling edge (detection 2, polarity bit), raw status on, TLMM interrupt itself kept disabled (bit 0 = 0)
    v = Rd(g_irq, TLMM_INTR_CFG);
    Wr(g_irq, TLMM_INTR_CFG, (v & ~0x1fu) | (2u << 2) | (1u << 1) | (1u << 4));
    Wr(g_irq, TLMM_INTR_STATUS, 0);
    Log("gpio86 intr_cfg %08lx -> %08lx", (unsigned long)v, (unsigned long)Rd(g_irq, TLMM_INTR_CFG));
    return 0;
}

static void LoadMap(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING name;
    HANDLE key;
    ULONG buf[8], got = 0;
    KEY_VALUE_PARTIAL_INFORMATION *pi = (KEY_VALUE_PARTIAL_INFORMATION *)buf;

    g_map = 3;
    if (!g_regpath.Buffer)
        return;
    InitializeObjectAttributes(&oa, &g_regpath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa)))
        return;
    RtlInitUnicodeString(&name, L"TouchMap");
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, 2, pi, sizeof(buf), &got)) && pi->Type == REG_DWORD && pi->DataLength == 4)
        g_map = *(ULONG *)pi->Data & 7;
    ZwClose(key);
}

// =====================================================================================================================================
// HID: report descriptor (Touch Screen, 10 contacts in parallel mode) and the read queue

#define NFING   10
#define XMAX    1199
#define YMAX    1999
#define REPLEN  (1 + 6 * NFING + 1)     // id, 10 x (flags, contact id, x16, y16), contact count

#define FINGER \
    0x09, 0x22, 0xA1, 0x02,                         /* Usage (Finger), Collection (Logical) */ \
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,   /* Tip Switch */ \
    0x09, 0x47, 0x81, 0x02,                         /* Confidence */ \
    0x95, 0x06, 0x81, 0x03,                         /* 6 bits padding */ \
    0x09, 0x51, 0x25, 0x0F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   /* Contact Identifier */ \
    0x05, 0x01, 0x75, 0x10, 0x55, 0x0E, 0x65, 0x11, /* Generic Desktop, 16 bit, unit 0.1 mm */ \
    0x26, 0xAF, 0x04, 0x46, 0xDC, 0x05, 0x09, 0x30, 0x81, 0x02,   /* X 0..1199 = 150 mm */ \
    0x26, 0xCF, 0x07, 0x46, 0xC4, 0x09, 0x09, 0x31, 0x81, 0x02,   /* Y 0..1999 = 250 mm */ \
    0x45, 0x00, 0x65, 0x00, 0x55, 0x00, 0x05, 0x0D, /* reset physical/unit, Digitizer page */ \
    0xC0

static const UCHAR g_rdesc[] = {
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,             // Digitizer, Touch Screen, Collection (Application)
    0x85, 0x01,                                     // Report ID 1
    FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER,
    0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,   // Contact Count
    0x85, 0x02, 0x09, 0x55, 0x25, 0x0A, 0xB1, 0x02, // Report ID 2 feature: Contact Count Maximum = 10
    0xC0
};

static KSPIN_LOCK g_lock;
static LIST_ENTRY g_reads;
#define RQ 16
static UCHAR g_ring[RQ][REPLEN];
static ULONG g_rhead, g_rcount;
static volatile LONG g_stop, g_running;
static PVOID g_thread;
static ULONG g_n_frames, g_n_bad, g_n_latch, g_n_level, g_n_poll, g_n_rep, g_n_done, g_n_drop, g_n_pend, g_n_cancel;

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
// event frames -> reports

static UCHAR g_prev_down[NFING];
static USHORT g_prev_x[NFING], g_prev_y[NFING];
static int g_csum_enforce = 1;
static ULONG g_csum_ok_seen;

static void MapPoint(ULONG rx, ULONG ry, USHORT *ox, USHORT *oy)
{
    ULONG x = rx, y = ry;

    if (g_map & 1)
        x = XMAX - x;
    if (g_map & 2)
        y = YMAX - y;
    if (g_map & 4) {                    // swap with rescale to the portrait ranges
        ULONG t = x;
        x = y * XMAX / YMAX;
        y = t * YMAX / XMAX;
    }
    *ox = (USHORT)x;
    *oy = (USHORT)y;
}

static void ProcessFrame(const UCHAR *ev, ULONG why)
{
    UCHAR rep[REPLEN], down[NFING];
    ULONG sum68 = 0, sum56 = 0, same = 0, i, n = 0, any = 0;
    int ok;

    for (i = 0; i < 68; i++) {
        sum68 += ev[i];
        if (i < 56)
            sum56 += ev[i];
        if (i < 56 && ev[i] == ev[0])
            same++;
    }
    g_n_frames++;
    ok = (sum68 & 0xff) == 0 || (sum56 & 0xff) == 0;
    if (g_n_frames <= 40) {
        char hex[150];
        int p = 0;
        for (i = 0; i < 68 && p < 140; i++)
            p += Fmt(hex + p, sizeof(hex) - p, "%02x", ev[i]);
        Log("frame %lu (%s) sum68 %02lx sum56 %02lx: %s", (unsigned long)g_n_frames, why == 1 ? "latch" : why == 2 ? "level" : "poll",
            (unsigned long)(sum68 & 0xff), (unsigned long)(sum56 & 0xff), hex);
    }
    if (same == 56) {                   // all-equal frame: exception event (EB/EC/EE), all zero or all FF
        g_n_bad++;
        return;
    }
    if (ok)
        g_csum_ok_seen++;
    else if (g_csum_enforce) {
        g_n_bad++;
        if (g_n_frames >= 20 && !g_csum_ok_seen) {
            g_csum_enforce = 0;
            Log("no frame passed the checksum in %lu frames: checksum check off", (unsigned long)g_n_frames);
        }
        return;
    }
    for (i = 0; i < NFING; i++) {
        ULONG x = ((ULONG)ev[4 * i] << 8) | ev[4 * i + 1], y = ((ULONG)ev[4 * i + 2] << 8) | ev[4 * i + 3];
        down[i] = x <= XMAX && y <= YMAX;
        if (down[i])
            MapPoint(x, y, &g_prev_x[i], &g_prev_y[i]);
    }
    for (i = 0; i < REPLEN; i++)
        rep[i] = 0;
    rep[0] = 1;
    for (i = 0; i < NFING; i++) {
        UCHAR *e;
        if (!down[i] && !g_prev_down[i])
            continue;
        e = rep + 1 + 6 * n;
        e[0] = down[i] ? 0x03 : 0x02;  // tip | confidence; lift = confidence only
        e[1] = (UCHAR)i;
        e[2] = (UCHAR)g_prev_x[i];
        e[3] = (UCHAR)(g_prev_x[i] >> 8);
        e[4] = (UCHAR)g_prev_y[i];
        e[5] = (UCHAR)(g_prev_y[i] >> 8);
        n++;
        any |= down[i];
        g_prev_down[i] = down[i];
    }
    rep[REPLEN - 1] = (UCHAR)n;
    if (n) {
        if (g_n_rep < 60)
            Log("report: %lu contacts, first id %u tip %u at %u,%u", (unsigned long)n, rep[2], rep[1] & 1, rep[3] | (rep[4] << 8),
                rep[5] | (rep[6] << 8));
        QueueReport(rep);
    }
    (void)any;
}

static int AnyDown(void)
{
    int i;
    for (i = 0; i < NFING; i++)
        if (g_prev_down[i])
            return 1;
    return 0;
}

static VOID TouchThread(PVOID ctx)
{
    UCHAR ev[128];
    ULONG idle = 0, ticks = 0, stats = 0, quiet = 0, why, i;

    UNREFERENCED_PARAMETER(ctx);
    ExSetTimerResolution(10000, TRUE);  // 1 ms, so SleepMs(2) is ~2 ms and not 15.6 ms
    LoadMap();
    Log("=== hidtouch thread: TouchMap %lu", (unsigned long)g_map);
    for (i = 0; i < NFING; i++)
        g_prev_down[i] = 0;
    if (HwInit()) {
        Log("hardware init failed; thread ends");
        goto out;
    }
    Log("touch running");
    while (!g_stop) {
        why = 0;
        if (Rd(g_irq, TLMM_INTR_STATUS) & 1) {
            Wr(g_irq, TLMM_INTR_STATUS, 0);
            why = 1;
            g_n_latch++;
        } else if (!(Rd(g_irq, 4) & 1)) {
            why = 2;
            g_n_level++;
        } else if (AnyDown() && ++idle >= 10) {
            why = 3;                    // a finger is down but nothing for ~20 ms: read anyway (missed edge)
            g_n_poll++;
        }
        if (why) {
            idle = 0;
            quiet = 0;
            HxRead(0x30, ev, 128);
            ProcessFrame(ev, why);
        } else if (AnyDown() && ++quiet >= 500) {
            // ~1 s without any frame while a contact is down: release everything
            UCHAR none[128];
            for (i = 0; i < 128; i++)
                none[i] = 0xFF;
            none[0] = 0xFE;             // not an all-equal frame; points stay invalid
            Log("watchdog: no frames for 1 s with a contact down, releasing");
            g_csum_enforce = 0;
            ProcessFrame(none, 3);
            g_csum_enforce = g_csum_ok_seen ? 1 : 0;
            quiet = 0;
        }
        SleepMs(2);
        if (++ticks >= 5000 && stats < 40) {   // ~10 s
            ticks = 0;
            stats++;
            Log("stats: frames %lu bad %lu latch %lu level %lu poll %lu reports %lu done %lu drop %lu pended %lu cancel %lu",
                (unsigned long)g_n_frames, (unsigned long)g_n_bad, (unsigned long)g_n_latch, (unsigned long)g_n_level, (unsigned long)g_n_poll,
                (unsigned long)g_n_rep, (unsigned long)g_n_done, (unsigned long)g_n_drop, (unsigned long)g_n_pend, (unsigned long)g_n_cancel);
        }
    }
out:
    ExSetTimerResolution(0, FALSE);
    Log("hidtouch thread exits");
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS StartHw(void)
{
    HANDLE h;
    NTSTATUS st;

    if (g_thread)
        return STATUS_SUCCESS;
    g_stop = 0;
    st = PsCreateSystemThread(&h, 0x1FFFFF, NULL, NULL, NULL, TouchThread, NULL);
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
        a.ProductID = 0x606F;
        a.VersionNumber = 0xD403;       // touch FW version
        for (i = 0; i < 11; i++)
            a.Reserved[i] = 0;
        Copy(ub, (const UCHAR *)&a, sizeof(a));
        return Done(irp, STATUS_SUCCESS, sizeof(a));
    }
    case IOCTL_HID_READ_REPORT:
        if (g_stop)
            return Done(irp, STATUS_DELETE_PENDING, 0);
        return PendRead(irp);
    case IOCTL_HID_GET_FEATURE: {
        HID_XFER_PACKET *p = (HID_XFER_PACKET *)irp->UserBuffer;
        if (!p || !p->reportBuffer || p->reportId != 2 || p->reportBufferLen < 2)
            return Done(irp, STATUS_INVALID_PARAMETER, 0);
        p->reportBuffer[0] = 2;
        p->reportBuffer[1] = NFING;
        return Done(irp, STATUS_SUCCESS, 2);
    }
    case IOCTL_HID_GET_STRING: {
        static const WCHAR man[] = L"Lenovo", prod[] = L"J606F Himax HX83102E touch screen";
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
    ULONG i, n;

    Log("=== j606f hidtouch: DriverEntry");
    g_lock = 0;
    InitializeListHead(&g_reads);
    n = reg && reg->Buffer ? reg->Length / 2 : 0;
    if (n > 259)
        n = 259;
    for (i = 0; i < n; i++)
        g_regbuf[i] = reg->Buffer[i];
    g_regbuf[n] = 0;
    g_regpath.Buffer = n ? g_regbuf : NULL;
    g_regpath.Length = (USHORT)(n * 2);
    g_regpath.MaximumLength = sizeof(g_regbuf);

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
