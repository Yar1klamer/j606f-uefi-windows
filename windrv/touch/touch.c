// J606F touch, step 1 (Windows ARM64 kernel driver, test-signed): bring up QUP0 SE2 as a GENI SPI master in FIFO mode and read the
// Himax HX83102E IC ID (register 0x900000D0, expected 83 10 2E). No reset, no firmware, nothing written to the touch IC except its
// AHB "register read" window commands. Log: C:\j606f\out\touch.txt (write-through).
// Facts (pmOS port, 2026-10-29; sm6115.dtsi; gcc-sm6115.c; pinctrl-sm6115.c; spi-geni-qcom.c; qcom-geni-se.c; himax83102p driver):
//   SPI2 = QUP0 SE2 @ 0x04A88000, CS0, mode 1 (CPHA), 10 MHz max; pins gpio6/7/71/80 function 1 (qup2), WEST tile; touch IRQ gpio86
//   (WEST, falling edge), reset gpio54 (EAST, not touched here). GCC: vote 0x7900c bit 12 (S2), CBCR 0x1f3a4, RCGR 0x1f3a8.
//   pmOS: SE0 (I2C) holds GPI DMA channels, SE2 (SPI) does not → SE2 runs in FIFO mode.
//   Himax SPI: write = F2 cmd data..., read = F3 cmd 00 then data (one CS). Register read: 13←31, 0D←10, 00←addr LE, 0C←00, read 08 ×4.
#include "j606f_nt.h"

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
    RtlInitUnicodeString(&path, L"\\??\\C:\\j606f\\out\\touch.txt");
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
    DbgPrint("j606f touch: %s", line);
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

static void PinLevels(volatile UCHAR **g, const char *tag)
{
    Log("%s: pad in gpio6 %lu gpio7 %lu gpio71 %lu gpio80 %lu gpio86 %lu; SE2 GENI_IOS %08lx", tag, (unsigned long)(Rd(g[0], 4) & 1),
        (unsigned long)(Rd(g[1], 4) & 1), (unsigned long)(Rd(g[2], 4) & 1), (unsigned long)(Rd(g[3], 4) & 1),
        (unsigned long)(Rd(g[4], 4) & 1), (unsigned long)Rd(g_se, 0x908));
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

static void Probe(const char *what)
{
    UCHAR b1 = 0x31, id[4];
    int rc;

    HxWrite(0x13, &b1, 1);
    b1 = 0;
    rc = HxRead(0x13, &b1, 1);
    Log("%s: cpha %lu cpol %lu clk_cfg %08lx style %d fix %d: read 0x13 (wrote 31) rc %d, %02x", what, (unsigned long)Rd(g_se, SE_SPI_CPHA),
        (unsigned long)Rd(g_se, SE_SPI_CPOL) >> 2, (unsigned long)Rd(g_se, GENI_SER_M_CLK_CFG), g_rdstyle, g_fixshift, rc, b1);
    HxRegReadRaw(0x900000D0, id, "  IC ID");
    SleepMs(5);
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

static void Run(void)
{
    volatile UCHAR *vote, *qup, *se, *g[5], *rst;
    static const ULONG pins[5] = { 6, 7, 71, 80, 86 };
    ULONG v, fw, p0, p1;
    UCHAR id[4] = { 0 }, b1 = 0;
    int i, rc;

    vote = Map(GCC_VOTE_PA, 0x1000);
    qup = Map(GCC_QUP_PA, 0x1000);
    for (i = 0; i < 5; i++)
        g[i] = Map(TLMM_WEST_PA + 0x1000 * pins[i], 0x1000);
    se = Map(SE2_PA, SE_SIZE);
    if (!vote || !qup || !g[0] || !g[1] || !g[2] || !g[3] || !g[4] || !se)
        return;
    g_se = se;
    g_qup = qup;

    Log("GCC vote %08lx; CBCR m_ahb %08lx s_ahb %08lx core %08lx s2 %08lx; S2 RCGR cmd %08lx cfg %08lx", (unsigned long)Rd(vote, VOTE_REG),
        (unsigned long)Rd(qup, 0x004), (unsigned long)Rd(qup, 0x008), (unsigned long)Rd(qup, 0x00c), (unsigned long)Rd(qup, S2_CBCR),
        (unsigned long)Rd(qup, S2_RCGR_CMD), (unsigned long)Rd(qup, S2_RCGR_CFG));
    for (i = 0; i < 5; i++)
        Log("gpio%lu cfg %08lx io %08lx", (unsigned long)pins[i], (unsigned long)Rd(g[i], 0), (unsigned long)Rd(g[i], 4));

    Wr(vote, VOTE_REG, Rd(vote, VOTE_REG) | VOTE_BITS);
    for (i = 0; i < 100; i++) {
        if (!(Rd(qup, 0x004) & CBCR_OFF) && !(Rd(qup, 0x008) & CBCR_OFF) && !(Rd(qup, S2_CBCR) & CBCR_OFF))
            break;
        SleepMs(1);
    }
    Log("after vote (%d ms): CBCR m_ahb %08lx s_ahb %08lx s2 %08lx", i, (unsigned long)Rd(qup, 0x004), (unsigned long)Rd(qup, 0x008),
        (unsigned long)Rd(qup, S2_CBCR));
    if (i == 100) {
        Log("S2 clocks did not come up; stop");
        return;
    }
    v = Rd(qup, S2_RCGR_CFG);
    if (v & 0x371f) {                   // source XO 19.2 MHz, no divider, no MND
        Wr(qup, S2_RCGR_CFG, v & ~0x371fu);
        Wr(qup, S2_RCGR_CMD, Rd(qup, S2_RCGR_CMD) | 1);
        for (i = 0; i < 100 && (Rd(qup, S2_RCGR_CMD) & 1); i++)
            SleepMs(1);
        Log("S2 RCGR cfg %08lx -> %08lx (cmd %08lx)", (unsigned long)v, (unsigned long)Rd(qup, S2_RCGR_CFG), (unsigned long)Rd(qup, S2_RCGR_CMD));
    }
    for (i = 0; i < 4; i++) {           // SPI pins as in pmOS: function 1 (qup2), no pull, 6 mA (drive code 2)
        v = Rd(g[i], 0);
        Wr(g[i], 0, (1u << 2) | (2u << 6));
        Log("gpio%lu cfg %08lx -> %08lx", (unsigned long)pins[i], (unsigned long)v, (unsigned long)Rd(g[i], 0));
    }
    v = Rd(g[4], 0);                    // touch IRQ gpio86 as in pmOS: GPIO input, pull-up, 8 mA
    Wr(g[4], 0, 3u | (3u << 6));
    Log("gpio86 cfg %08lx -> %08lx", (unsigned long)v, (unsigned long)Rd(g[4], 0));
    rst = Map(TLMM_EAST_PA + 0x1000 * 54, 0x1000);
    if (!rst)
        return;
    Log("gpio54 (touch reset) cfg %08lx io %08lx", (unsigned long)Rd(rst, 0), (unsigned long)Rd(rst, 4));

    fw = Rd(se, GENI_FW_REVISION_RO);
    p0 = Rd(se, SE_HW_PARAM_0);
    p1 = Rd(se, SE_HW_PARAM_1);
    Log("SE2: FW_REVISION %08lx (protocol %lu), IF_DISABLE %08lx, HW_PARAM0 %08lx, HW_PARAM1 %08lx, STATUS %08lx", (unsigned long)fw,
        (unsigned long)((fw >> 8) & 0xff), (unsigned long)Rd(se, GENI_IF_DISABLE_RO), (unsigned long)p0, (unsigned long)p1,
        (unsigned long)Rd(se, SE_GENI_STATUS));
    if (((fw >> 8) & 0xff) != 1) {
        Log("SE2 firmware is not SPI (1); stop");
        return;
    }
    if (Rd(se, GENI_IF_DISABLE_RO) & 1) {
        Log("SE2 FIFO disabled (GPI DMA only); stop");
        return;
    }
    v = (p0 >> 16) & 0x3f;              // TX FIFO depth (words)
    if (v >= 4)
        g_fifo_depth = v;

    // geni_se_init(se, 0, depth - 2) + FIFO mode (qcom-geni-se.c)
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
    // SPI mode 1, CS0, 8-bit words packed 4 per FIFO word MSB first (geni_se_config_packing(8, 4, true)), 19.2 MHz / 4 = 4.8 MHz
    Wr(se, SE_SPI_LOOPBACK, 0);
    Wr(se, SE_SPI_DEMUX_SEL, 0);
    Wr(se, SE_SPI_CPHA, 1);
    Wr(se, SE_SPI_CPOL, 0);
    Wr(se, SE_SPI_DEMUX_OUTPUT_INV, 0);
    Wr(se, SE_SPI_TRANS_CFG, Rd(se, SE_SPI_TRANS_CFG) & ~(1u << 1));
    Wr(se, SE_GENI_TX_PACKING_CFG0, 0x7f8fe);
    Wr(se, SE_GENI_TX_PACKING_CFG1, 0xffefe);
    Wr(se, SE_GENI_RX_PACKING_CFG0, 0x7f8fe);
    Wr(se, SE_GENI_RX_PACKING_CFG1, 0xffefe);
    Wr(se, SE_GENI_BYTE_GRAN, 0);
    Wr(se, SE_SPI_WORD_LEN, 8 - 4);
    Wr(se, SE_GENI_CLK_SEL, 0);
    Wr(se, GENI_SER_M_CLK_CFG, (4u << 4) | 1);
    Log("SE2 SPI set up: FIFO depth %lu words, 4.8 MHz, mode 1", (unsigned long)g_fifo_depth);

    g_rst = rst;
    g_split = 1;
    g_rdstyle = 0;
    g_fixshift = 0;
    // run_16: correct data only with CPOL 1 (both CPHA); use CPHA 1 + CPOL 1, SE 100 MHz, SPI 10 MHz (div 10) as pmOS
    if (!SetSeClk(1)) {
        Log("SE2 100 MHz failed; stop");
        return;
    }
    SetMode(1, 1, 10);

    // chip_detect
    PinReset(5, 50);
    rc = HxRead(0x13, &b1, 1);
    Log("bus read 0x13: rc %d, %02x", rc, b1);
    Log("sense_off: %s", SenseOff() ? "ok (safe mode 0x0C)" : "FAILED");
    HxRegReadRaw(0x900000D0, id, "IC ID");
    if (!(id[3] == 0x83 && id[2] == 0x10 && id[1] == 0x2E)) {
        Log("not HX83102E; stop");
        return;
    }
    Log("detect IC HX83102E");

    // zero-flash firmware upload
    {
        ULONG size = 0;
        UCHAR *fw = LoadFile(L"\\??\\C:\\j606f\\drv\\hxfw.bin", &size);
        UCHAR ev[128], last[64];
        ULONG t, lines = 0, changed;
        int up;

        Log("firmware: %lu bytes", (unsigned long)size);
        if (!fw || size != 131072) {
            Log("firmware missing or wrong size; stop");
            if (fw)
                ExFreePoolWithTag(fw, 0x66363036);
            return;
        }
        up = ZfUpload(fw, size);
        ExFreePoolWithTag(fw, 0x66363036);
        Log("zf upload: %d", up);

        // FW info (himax_mcu_read_FW_ver / touch_information)
        {
            UCHAR d[8] = { 0 };
            RegReadN(0x10007004, d, 4);
            Log("FW ver reg 10007004: %02x %02x %02x %02x", d[0], d[1], d[2], d[3]);
            RegReadN(0x10007084, d, 4);
            Log("CFG ver reg 10007084: %02x %02x %02x %02x", d[0], d[1], d[2], d[3]);
            RegReadN(0x100070F4, d, 8);
            Log("rx/tx/maxpt 100070f4: %02x %02x %02x %02x %02x %02x %02x %02x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            RegReadN(0x100070F8, d, 8);
            Log("xy res 100070f8: %02x %02x %02x %02x %02x %02x %02x %02x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
            RegReadN(0x10007088, d, 4);
            Log("int is edge / fw mode 10007088: %02x %02x %02x %02x", d[0], d[1], d[2], d[3]);
            Log("central state %08lx, gpio86 %lu", (unsigned long)RegRd32(0x900000A8), (unsigned long)(Rd(g[4], 4) & 1));
        }

        // 3) events for 90 s: when the IRQ line (gpio86) is low, read the event stack (0x30 x 128)
        for (i = 0; i < 64; i++)
            last[i] = 0;
        for (t = 0; t < 90000 && lines < 400; t += 5) {
            if (Rd(g[4], 4) & 1) {
                SleepMs(5);
                continue;
            }
            HxRead(0x30, ev, 128);          // hx83102e_read_event_stack: plain bus read, no AHB burst toggling
            changed = 0;
            for (i = 0; i < 64; i++)
                if (ev[i] != last[i]) { changed = 1; last[i] = ev[i]; }
            if (changed) {
                char pts[120];
                int pn = 0, k;
                pts[0] = 0;
                for (k = 0; k < 10 && pn < 100; k++) {
                    ULONG x = ((ULONG)ev[4 * k] << 8) | ev[4 * k + 1], y = ((ULONG)ev[4 * k + 2] << 8) | ev[4 * k + 3];
                    if (x != 0xFFFF && y != 0xFFFF && pn < 100)
                        pn += Fmt(pts + pn, sizeof(pts) - pn, "%d:%lu,%lu ", k, (unsigned long)x, (unsigned long)y);
                }
                Log("t=%lu ms ev %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x | %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x | pts %s",
                    (unsigned long)t, ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7], ev[8], ev[9], ev[10], ev[11], ev[12], ev[13], ev[14],
                    ev[15], ev[40], ev[41], ev[42], ev[43], ev[44], ev[45], ev[46], ev[47], ev[48], ev[49], ev[50], ev[51], pts);
                lines++;
            }
            SleepMs(5);
        }
        Log("event loop end: %lu lines", (unsigned long)lines);
    }
    PinLevels(g, "end");
}

static VOID TouchUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = TouchUnload;
    Log("=== j606f touch step 2b (zero-flash FW upload with hx83102e sense_on/reload_to_active, events for 90 s)");
    Run();
    Log("=== j606f touch end");
    return STATUS_SUCCESS;
}
