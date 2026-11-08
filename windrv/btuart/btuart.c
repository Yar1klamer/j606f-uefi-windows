// J606F Bluetooth step 2: WCN3990 on UART3 (QUP0 SE3, 0x4a8c000) from Windows (ARM64 kernel driver, test-signed, legacy service).
// 1. RPM (GLINK, ../rpm/rpm.c): BT LDOs on — ldoa9 1.8 V (vddio), ldoa16 1.704 V (vddxo), ldoa17 1.152 V (vddrf), ldoa23 3.2 V (vddch0) (run_41: acked).
// 2. Clocks: GCC vote 0x7900c bits 6/7/8/9 (QUP0 m/s AHB, core, core2x — as hidtouch) + 13 (S3); RCGR S3 0x1f4d8 = 7.3728 MHz
//    (GPLL0_OUT_AUX2 300 MHz * 384/15625: M 0x180, N/D 0xc476, CFG src 2 | pre_div 1 | dual edge) — gcc-sm6115.c / clk-rcg2.c.
// 3. Pins gpio8..11 (TLMM east, func 1 qup3: CTS, RTS, TX, RX), RX pull-up.
// 4. GENI UART in FIFO mode as qcom_geni_serial.c (8N1, CTS ignored, RFR forced ready, packing 8 bit lsb first 4/word),
//    oversampling 32 (QUP < 2.5) or 16.
// 5. WCN3990 power-up as hci_qca.c: 2400 bps -> 0xC0 (off pulse), 115200 bps -> 0xFC (on pulse), 100 ms; then HCI over H4:
//    vendor EDL 0xFC00 [0x19] (patch/SoC version), HCI_Reset 0x0C03, Read_Local_Version 0x1001. Every received byte is logged.
// 6. Unload: off pulse 0xC0 at 2400, LDOs off, RPM channel CLOSE.
// 7. (run_43) Firmware download in a system thread: rampatch C:\j606f\drv\crbtfw21.tlv + NVM crnv21.bin (btqca.c), HCI_Reset, version,
//    features, buffer size, a 6.4 s inquiry (class/RSSI only, no addresses logged). Log: C:\j606f\out\btuart.txt.
#define RPM_NO_ENTRY
#ifndef RPM_LOG_PATH
#define RPM_LOG_PATH L"\\??\\C:\\j606f\\out\\btuart.txt"
#endif
#include "../rpm/rpm.c"

#define GCC_PA          0x01400000
#define GCC_SIZE        0x80000
#define VOTE            0x7900c
#define VOTE_BITS       ((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 13))
#define QUP_M_AHB_CBCR  0x1f004
#define QUP_S_AHB_CBCR  0x1f008
#define S3_CBCR         0x1f4d4
#define S3_RCGR         0x1f4d8
#define CBCR_OFF        (1u << 31)
#define TLMM_EAST       0x00d00000
#define QUP0_WRAP_PA    0x04ac0000
#define SE3_PA          0x04a8c000
#define SE_SIZE         0x4000

#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define SE_GENI_CGC_CTRL        0x28
#define SE_GENI_STATUS          0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_SER_S_CLK_CFG      0x4c
#define GENI_IF_DISABLE_RO      0x64
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_UART_LOOPBACK_CFG    0x22c
#define SE_UART_TX_TRANS_CFG    0x25c
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BIT_LEN 0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE_CNT    0x294
#define SE_UART_TX_PARITY_CFG   0x2a4
#define SE_UART_RX_PARITY_CFG   0x2a8
#define SE_UART_MANUAL_RFR      0x2ac
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_CMD_CTRL_REG  0x634
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_EN        0x644
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_TX_FIFO_STATUS  0x800
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WM       0x814
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_HW_PARAM_1           0xe28
#define SE_DMA_GENERAL_CFG      0xe30

static volatile UCHAR *g_gcc, *g_se, *g_pin[4];
static ULONG g_sampling = 32, g_rx_depth = 16;
static int g_uart_ok;

static int ClocksOn(void)
{
    int i;
    ULONG cfg;

    Wr(g_gcc, VOTE, Rd(g_gcc, VOTE) | VOTE_BITS);
    for (i = 0; i < 100; i++) {
        if (!(Rd(g_gcc, QUP_M_AHB_CBCR) & CBCR_OFF) && !(Rd(g_gcc, QUP_S_AHB_CBCR) & CBCR_OFF) && !(Rd(g_gcc, S3_CBCR) & CBCR_OFF))
            break;
        SleepMs(1);
    }
    Log("clocks: vote %08lx m_ahb %08lx s_ahb %08lx s3 %08lx (%d ms)", (unsigned long)Rd(g_gcc, VOTE), (unsigned long)Rd(g_gcc, QUP_M_AHB_CBCR),
        (unsigned long)Rd(g_gcc, QUP_S_AHB_CBCR), (unsigned long)Rd(g_gcc, S3_CBCR), i);
    if (i == 100)
        return -1;
    // RCGR: CMD +0, CFG +4, M +8, N +c, D +10
    Log("s3 rcgr before: cmd %08lx cfg %08lx m %08lx n %08lx d %08lx", (unsigned long)Rd(g_gcc, S3_RCGR), (unsigned long)Rd(g_gcc, S3_RCGR + 4),
        (unsigned long)Rd(g_gcc, S3_RCGR + 8), (unsigned long)Rd(g_gcc, S3_RCGR + 0xc), (unsigned long)Rd(g_gcc, S3_RCGR + 0x10));
    Wr(g_gcc, S3_RCGR + 8, 0x180);
    Wr(g_gcc, S3_RCGR + 0xc, 0xc476);
    Wr(g_gcc, S3_RCGR + 0x10, 0xc476);
    cfg = Rd(g_gcc, S3_RCGR + 4) & ~0x371fu;
    Wr(g_gcc, S3_RCGR + 4, cfg | (2u << 12) | (2u << 8) | 1u);
    Wr(g_gcc, S3_RCGR, Rd(g_gcc, S3_RCGR) | 1);
    for (i = 0; i < 100 && (Rd(g_gcc, S3_RCGR) & 1); i++)
        SleepMs(1);
    Log("s3 rcgr after: cmd %08lx cfg %08lx m %08lx n %08lx d %08lx (%d ms) cbcr %08lx", (unsigned long)Rd(g_gcc, S3_RCGR),
        (unsigned long)Rd(g_gcc, S3_RCGR + 4), (unsigned long)Rd(g_gcc, S3_RCGR + 8), (unsigned long)Rd(g_gcc, S3_RCGR + 0xc),
        (unsigned long)Rd(g_gcc, S3_RCGR + 0x10), i, (unsigned long)Rd(g_gcc, S3_CBCR));
    return (Rd(g_gcc, S3_RCGR) & 0x80000000) ? -1 : 0;     // ROOT_OFF
}

static void SetBaud(ULONG baud)
{
    ULONG div = 7372800 / (baud * g_sampling);

    if (!div)
        div = 1;
    Wr(g_se, GENI_SER_M_CLK_CFG, (div << 4) | 1);
    Wr(g_se, GENI_SER_S_CLK_CFG, (div << 4) | 1);
    Log("baud %lu: div %lu (actual %lu)", (unsigned long)baud, (unsigned long)div, (unsigned long)(7372800 / (div * g_sampling)));
}

static int UartInit(void)
{
    volatile UCHAR *wrap = Map(QUP0_WRAP_PA, 0x1000);
    ULONG fw, ver = 0, p0;
    int i;

    if (wrap) {
        ver = Rd(wrap, 4);
        MmUnmapIoSpace((PVOID)wrap, 0x1000);
    }
    g_sampling = ver >= 0x20050000 ? 16 : 32;
    for (i = 0; i < 4; i++)
        Wr(g_pin[i], 0, (1u << 2) | (i == 3 ? 3u : 0u));      // func 1, 2 mA, RX pull-up
    fw = Rd(g_se, GENI_FW_REVISION_RO);
    p0 = Rd(g_se, SE_HW_PARAM_0);
    Log("se3: qup hw ver %08lx (sampling %lu) fw_rev %08lx (proto %lu) if_disable %08lx hw_param0 %08lx param1 %08lx status %08lx",
        (unsigned long)ver, (unsigned long)g_sampling, (unsigned long)fw, (unsigned long)((fw >> 8) & 0xff),
        (unsigned long)Rd(g_se, GENI_IF_DISABLE_RO), (unsigned long)p0, (unsigned long)Rd(g_se, SE_HW_PARAM_1), (unsigned long)Rd(g_se, SE_GENI_STATUS));
    if (((fw >> 8) & 0xff) != 2 || (Rd(g_se, GENI_IF_DISABLE_RO) & 1)) {
        Log("se3 is not a FIFO UART");
        return -1;
    }
    if (((Rd(g_se, SE_HW_PARAM_1) >> 16) & 0x3f) >= 4)
        g_rx_depth = (Rd(g_se, SE_HW_PARAM_1) >> 16) & 0x3f;

    // geni_se_irq_clear + io_init + io_set_mode
    Wr(g_se, SE_GSI_EVENT_EN, 0);
    Wr(g_se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    Wr(g_se, SE_GENI_S_IRQ_CLEAR, 0xffffffff);
    Wr(g_se, SE_DMA_TX_IRQ_CLR, 0xffffffff);
    Wr(g_se, SE_DMA_RX_IRQ_CLR, 0xffffffff);
    Wr(g_se, SE_IRQ_EN, 0xffffffff);
    Wr(g_se, SE_GENI_CGC_CTRL, Rd(g_se, SE_GENI_CGC_CTRL) | 0x7f);
    Wr(g_se, SE_DMA_GENERAL_CFG, Rd(g_se, SE_DMA_GENERAL_CFG) | 0xf);
    Wr(g_se, GENI_OUTPUT_CTRL, 0x7f);
    Wr(g_se, GENI_FORCE_DEFAULT_REG, 1);
    Wr(g_se, SE_IRQ_EN, Rd(g_se, SE_IRQ_EN) | 0xf);
    Wr(g_se, SE_GENI_DMA_MODE_EN, Rd(g_se, SE_GENI_DMA_MODE_EN) & ~1u);
    Wr(g_se, SE_GSI_EVENT_EN, 0);
    Wr(g_se, SE_GENI_RX_WATERMARK, 2);
    Wr(g_se, SE_GENI_RX_RFR_WM, g_rx_depth - 2);
    // packing: 8 bits per char, 4 chars per word, lsb first (geni_se_config_packing(8, 4, false))
    Wr(g_se, SE_GENI_TX_PACKING_CFG0, 0x4380e);
    Wr(g_se, SE_GENI_TX_PACKING_CFG1, 0xc3e0e);
    Wr(g_se, SE_GENI_RX_PACKING_CFG0, 0x4380e);
    Wr(g_se, SE_GENI_RX_PACKING_CFG1, 0xc3e0e);
    Wr(g_se, SE_UART_RX_STALE_CNT, 160);
    // termios: 8N1, CTS ignored (UART_CTS_MASK), no loopback
    Wr(g_se, SE_UART_LOOPBACK_CFG, 0);
    Wr(g_se, SE_UART_TX_TRANS_CFG, 1u << 1);
    Wr(g_se, SE_UART_TX_PARITY_CFG, 0);
    Wr(g_se, SE_UART_RX_TRANS_CFG, 0);
    Wr(g_se, SE_UART_RX_PARITY_CFG, 0);
    Wr(g_se, SE_UART_TX_WORD_LEN, 8);
    Wr(g_se, SE_UART_RX_WORD_LEN, 8);
    Wr(g_se, SE_UART_TX_STOP_BIT_LEN, 0);
    Wr(g_se, SE_GENI_CLK_SEL, 0);
    Wr(g_se, SE_UART_MANUAL_RFR, (1u << 31) | 1u);     // manual RFR, ready (RTS asserted)
    SetBaud(115200);
    return 0;
}

static int UartTx(const UCHAR *b, ULONG n)
{
    ULONG i, w;
    int t;

    Wr(g_se, SE_GENI_M_IRQ_CLEAR, 0xffffffff);
    Wr(g_se, SE_UART_TX_TRANS_LEN, n);
    Wr(g_se, SE_GENI_M_CMD0, 1u << 27);                 // UART_START_TX
    for (i = 0; i < n; i += 4) {
        ULONG k;
        for (t = 0; t < 1000 && (Rd(g_se, SE_GENI_TX_FIFO_STATUS) & 0x0fffffff) >= 14; t++)
            KeStallExecutionProcessor(10);
        w = 0;
        for (k = 0; k < 4 && i + k < n; k++)
            w |= (ULONG)b[i + k] << (8 * k);
        Wr(g_se, SE_GENI_TX_FIFOn, w);
    }
    for (t = 0; t < 20000; t++) {                        // 2 s
        if (Rd(g_se, SE_GENI_M_IRQ_STATUS) & 1)
            break;
        KeStallExecutionProcessor(100);
    }
    w = Rd(g_se, SE_GENI_M_IRQ_STATUS);
    Wr(g_se, SE_GENI_M_IRQ_CLEAR, w);
    if (!(w & 1)) {
        Log("tx %lu bytes: no CMD_DONE (m_irq %08lx status %08lx)", (unsigned long)n, (unsigned long)w, (unsigned long)Rd(g_se, SE_GENI_STATUS));
        Wr(g_se, SE_GENI_M_CMD_CTRL_REG, 1u << 2);        // cancel
        SleepMs(5);
        return -1;
    }
    return 0;
}

static void RxStart(void)
{
    Wr(g_se, SE_GENI_S_IRQ_CLEAR, 0xffffffff);
    Wr(g_se, SE_GENI_S_CMD0, 1u << 27);                 // UART_START_READ
}

static void RxStop(void)
{
    Wr(g_se, SE_GENI_S_CMD_CTRL_REG, 1u << 1);          // abort
    SleepMs(2);
    Wr(g_se, SE_GENI_S_IRQ_CLEAR, 0xffffffff);
}

// collect bytes for ms milliseconds (wall clock); logs them in hex
void KeQuerySystemTimePrecise(PLARGE_INTEGER CurrentTime);

static ULONG RxCollect(UCHAR *out, ULONG max, int ms, const char *tag)
{
    static const char hex[] = "0123456789abcdef";
    ULONG got = 0, n = 0, i;
    LARGE_INTEGER now, end;
    char line[200];

    KeQuerySystemTimePrecise(&end);
    end.QuadPart += 10000LL * ms;
    do {
        ULONG st = Rd(g_se, SE_GENI_RX_FIFO_STATUS), wc = st & 0x1ffffff;
        for (i = 0; i < wc; i++) {
            ULONG w = Rd(g_se, SE_GENI_RX_FIFOn), nb = 4, k;
            if (i == wc - 1 && (st & (1u << 31))) {
                nb = (st >> 28) & 7;
                if (!nb)
                    nb = 4;
            }
            for (k = 0; k < nb; k++)
                if (got < max)
                    out[got++] = (UCHAR)(w >> (8 * k));
        }
        if (!wc)
            KeStallExecutionProcessor(200);
        Wr(g_se, SE_GENI_S_IRQ_CLEAR, Rd(g_se, SE_GENI_S_IRQ_STATUS));
        KeQuerySystemTimePrecise(&now);
    } while (now.QuadPart < end.QuadPart);
    for (i = 0; i < got && n + 4 < sizeof(line); i++) {
        line[n++] = ' ';
        line[n++] = hex[out[i] >> 4];
        line[n++] = hex[out[i] & 15];
    }
    line[n] = 0;
    Log("%s: %lu bytes:%s", tag, (unsigned long)got, line);
    return got;
}

// =====================================================================================================================================
// H4 receive: everything from the RX FIFO goes to g_rb; events (0x04) are taken out one by one.
#ifndef FILE_READ_DATA
#define FILE_READ_DATA 0x0001
#endif
#ifndef FILE_OPEN
#define FILE_OPEN 0x00000001
#endif
#define SE_GENI_IOS 0x908

static UCHAR g_rb[4096];
static ULONG g_rn, g_lost, g_junk;
static volatile LONG g_stop;
static PVOID g_thread;

static void HexLog(const char *tag, const UCHAR *b, ULONG n)
{
    static const char hex[] = "0123456789abcdef";
    char line[220];
    ULONG i, k = 0;

    for (i = 0; i < n && k + 4 < sizeof(line); i++) {
        line[k++] = ' ';
        line[k++] = hex[b[i] >> 4];
        line[k++] = hex[b[i] & 15];
    }
    line[k] = 0;
    Log("%s (%lu):%s", tag, (unsigned long)n, line);
}

static void RxDrain(void)
{
    ULONG st = Rd(g_se, SE_GENI_RX_FIFO_STATUS), wc = st & 0x1ffffff, i, k;

    for (i = 0; i < wc; i++) {
        ULONG w = Rd(g_se, SE_GENI_RX_FIFOn), nb = 4;
        if (i == wc - 1 && (st & (1u << 31))) {
            nb = (st >> 28) & 7;
            if (!nb)
                nb = 4;
        }
        for (k = 0; k < nb; k++) {
            if (g_rn < sizeof(g_rb))
                g_rb[g_rn++] = (UCHAR)(w >> (8 * k));
            else
                g_lost++;
        }
    }
    if (wc)
        Wr(g_se, SE_GENI_S_IRQ_CLEAR, Rd(g_se, SE_GENI_S_IRQ_STATUS));
}

static void RbShift(ULONG n)
{
    ULONG i;

    for (i = n; i < g_rn; i++)
        g_rb[i - n] = g_rb[i];
    g_rn -= n;
}

static LONGLONG NowUs(void)
{
    LARGE_INTEGER t;

    KeQuerySystemTimePrecise(&t);
    return t.QuadPart / 10;
}

// next HCI event: ev[0] = code, ev[1] = plen, ev[2..] = params; returns 2 + plen or 0 on timeout
static ULONG GetEvent(UCHAR *ev, ULONG max, int ms)
{
    LONGLONG end = NowUs() + 1000LL * ms;

    for (;;) {
        RxDrain();
        while (g_rn && g_rb[0] != 0x04) {
            if (g_junk < 16)
                Log("rx junk byte %02x", g_rb[0]);
            g_junk++;
            RbShift(1);
        }
        if (g_rn >= 3 && g_rn >= 3u + g_rb[2]) {
            ULONG n = 2u + g_rb[2], i;
            for (i = 0; i < n && i < max; i++)
                ev[i] = g_rb[1 + i];
            RbShift(n + 1);
            return n;
        }
        if (NowUs() >= end)
            return 0;
        KeStallExecutionProcessor(50);
    }
}

static int SendCmd(USHORT op, const UCHAR *p, ULONG plen)
{
    UCHAR c[4 + 255];
    ULONG i;

    c[0] = 1;
    c[1] = (UCHAR)op;
    c[2] = (UCHAR)(op >> 8);
    c[3] = (UCHAR)plen;
    for (i = 0; i < plen; i++)
        c[4 + i] = p[i];
    return UartTx(c, 4 + plen);
}

// send a command and wait for its Command Complete / Command Status; every event is logged (up to 64 bytes); returns CC/CS status or -1
static int Cmd(USHORT op, const UCHAR *p, ULONG plen, const char *tag, UCHAR *ev, ULONG evmax, int ms)
{
    UCHAR tmp[260];
    ULONG n;
    LONGLONG end;

    if (!ev) {
        ev = tmp;
        evmax = sizeof(tmp);
    }
    if (SendCmd(op, p, plen)) {
        Log("%s: tx failed", tag);
        return -1;
    }
    end = NowUs() + 1000LL * ms;
    while (NowUs() < end) {
        n = GetEvent(ev, evmax, 50);
        if (!n)
            continue;
        if (tag[0] != '!')
            HexLog(tag, ev, n > 64 ? 64 : n);
        if (ev[0] == 0x0e && n >= 6 && ev[3] == (UCHAR)op && ev[4] == (UCHAR)(op >> 8))
            return ev[5];
        if (ev[0] == 0x0f && n >= 6 && ev[4] == (UCHAR)op && ev[5] == (UCHAR)(op >> 8))
            return ev[2];
    }
    Log("%s: no command complete in %d ms", tag, ms);
    return -1;
}

// =====================================================================================================================================
// WCN3990 firmware download as btqca.c (qca_download_firmware / qca_tlv_send_segment): EDL 0xFC00 [0x1E, len, <= 243 bytes] per segment.
// Rampatch download mode 3 (QCA_SKIP_EVT_VSE_CC): only the last segment is answered (VSE ff: 00 04 <result>); NVM: every segment.
static UCHAR *LoadFile(const WCHAR *path, ULONG *len)
{
    UNICODE_STRING u;
    OBJECT_ATTRIBUTES a;
    IO_STATUS_BLOCK io;
    HANDLE f;
    UCHAR *b;
    NTSTATUS st;

    *len = 0;
    RtlInitUnicodeString(&u, path);
    InitializeObjectAttributes(&a, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&f, FILE_READ_DATA | SYNCHRONIZE, &a, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(st)) {
        Log("open fw file failed %08lx", (unsigned long)st);
        return NULL;
    }
    b = (UCHAR *)ExAllocatePool2(POOL_FLAG_NON_PAGED, 0x48000, 0x6674626a);
    if (b) {
        st = ZwReadFile(f, NULL, NULL, NULL, &io, b, 0x48000, NULL, NULL);
        if (NT_SUCCESS(st))
            *len = (ULONG)io.Information;
        else
            Log("read fw file failed %08lx", (unsigned long)st);
    }
    ZwClose(f);
    if (b && !*len) {
        ExFreePoolWithTag(b, 0x6674626a);
        b = NULL;
    }
    return b;
}

static ULONG Le32(const UCHAR *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24);
}

// btqca qca_tlv_check_data for NVM, adapted: no in-band sleep (HCI tag 17 data[0] bit7 cleared), UART stays 115200 (data[2] = 0),
// deep sleep off (tag 27 bit0 cleared) — the host side has no IBS yet. BD address tag 2 is only noted (not logged).
static void NvmPatch(UCHAR *d, ULONG size)
{
    ULONG type, length, idx = 0;
    UCHAR *t = d;

    if (size < 4)
        return;
    type = d[0];
    if (type == 4 && size >= 8)
        t = d + 4;
    type = t[0];
    length = Le32(t) >> 8;
    t += 4;
    Log("nvm: outer type %u, set type %lu len %lu", d[0], (unsigned long)type, (unsigned long)length);
    if (type != 2 || (ULONG)(t - d) + length > size)
        return;
    while (idx + 12 <= length) {
        UCHAR *n = t + idx;
        ULONG id = n[0] | (n[1] << 8), tl = n[2] | (n[3] << 8);
        if (idx + 12 + tl > length)
            break;
        if (id == 17 && tl >= 3) {
            Log("nvm tag 17 (hci): %02x %02x %02x -> in-band sleep off, baud 115200", n[12], n[13], n[14]);
            n[12] &= ~0x80;
            n[14] = 0;
        } else if (id == 27 && tl >= 1) {
            Log("nvm tag 27 (deep sleep): %02x -> off", n[12]);
            n[12] &= ~0x01;
        } else if (id == 2) {
            Log("nvm tag 2 (bd addr) present, len %lu", (unsigned long)tl);
        }
        idx += 12 + tl;
    }
}

static int SegWait(const char *what, ULONG seg)
{
    UCHAR ev[64];
    LONGLONG end = NowUs() + 3000000;

    while (NowUs() < end) {
        ULONG n = GetEvent(ev, sizeof(ev), 50);
        if (!n)
            continue;
        if (ev[0] == 0xff && n >= 5 && ev[2] == 0x00 && ev[3] == 0x04) {
            if (ev[4])
                Log("%s seg %lu: result %02x", what, (unsigned long)seg, ev[4]);
            return ev[4] ? -1 : 0;
        }
        HexLog("seg event", ev, n > 32 ? 32 : n);
    }
    Log("%s seg %lu: no VSE", what, (unsigned long)seg);
    return -1;
}

static int Download(const WCHAR *path, const char *what, int nvm)
{
    ULONG len, off = 0, seg = 0, mode = 0, acked = 0, extra = 0;
    UCHAR *fw = LoadFile(path, &len), c[2 + 243], ev[64];
    LONGLONG t0 = NowUs();
    int rc = 0;

    if (!fw)
        return -1;
    if (nvm)
        NvmPatch(fw, len);
    else {
        if (len < 24 || fw[0] != 1) {
            Log("%s: not a TLV patch", what);
            ExFreePoolWithTag(fw, 0x6674626a);
            return -1;
        }
        mode = fw[14];
        Log("%s: total %lu data %lu fmt %u sig %u dnld_mode %lu product %04x rom %04x patch %04x", what, (unsigned long)Le32(fw + 4),
            (unsigned long)Le32(fw + 8), fw[12], fw[13], (unsigned long)mode, fw[16] | (fw[17] << 8), fw[18] | (fw[19] << 8),
            fw[20] | (fw[21] << 8));
    }
    Log("%s: %lu bytes, %lu segments", what, (unsigned long)len, (unsigned long)((len + 242) / 243));
    while (off < len && !g_stop) {
        ULONG n = len - off > 243 ? 243 : len - off, i;
        int last = off + n >= len || n < 243;
        int wait = last || !(mode == 1 || mode == 3);
        c[0] = 0x1e;
        c[1] = (UCHAR)n;
        for (i = 0; i < n; i++)
            c[2 + i] = fw[off + i];
        if (SendCmd(0xfc00, c, 2 + n)) {
            Log("%s seg %lu: tx failed", what, (unsigned long)seg);
            rc = -1;
            break;
        }
        if (wait) {
            if (SegWait(what, seg)) {
                rc = -1;
                break;
            }
            acked++;
        } else {
            while (GetEvent(ev, sizeof(ev), 0)) {
                if (extra < 4)
                    HexLog("unexpected event during download", ev, ev[1] + 2 > 32 ? 32 : ev[1] + 2);
                extra++;
            }
        }
        off += n;
        seg++;
    }
    Log("%s: %s, %lu segments sent (%lu acked, %lu other events), %lu ms, rx lost %lu junk %lu", what, rc ? "FAILED" : "ok",
        (unsigned long)seg, (unsigned long)acked, (unsigned long)extra, (unsigned long)((NowUs() - t0) / 1000), (unsigned long)g_lost,
        (unsigned long)g_junk);
    ExFreePoolWithTag(fw, 0x6674626a);
    return rc;
}

// =====================================================================================================================================
static int g_ldo_on;

static void LdosOff(void)
{
    if (g_ldo_on) {
        Ldo(23, 0, 0);
        Ldo(17, 0, 0);
        Ldo(16, 0, 0);
        Ldo(9, 0, 0);
        g_ldo_on = 0;
    }
}

#ifndef BTUART_NO_ENTRY
static void Inquiry(void)
{
    static const UCHAR mode1[] = { 0x01 };
    static const UCHAR inq[] = { 0x33, 0x8b, 0x9e, 0x05, 0x00 };      // GIAC, 5 * 1.28 s, unlimited
    UCHAR ev[260];
    ULONG n, results = 0;
    LONGLONG end;

    Cmd(0x0c45, mode1, 1, "Write_Inquiry_Mode(rssi)", NULL, 0, 1000);
    if (Cmd(0x0401, inq, sizeof(inq), "Inquiry", NULL, 0, 1000) != 0)
        return;
    end = NowUs() + 9000000;
    while (NowUs() < end && !g_stop) {
        n = GetEvent(ev, sizeof(ev), 100);
        if (!n)
            continue;
        if (ev[0] == 0x01) {
            Log("inquiry complete, status %02x, %lu results", ev[2], (unsigned long)results);
            return;
        }
        if ((ev[0] == 0x22 || ev[0] == 0x02 || ev[0] == 0x2f) && n >= 17) {
            // addresses are not logged; class of device and RSSI only
            UCHAR *r = ev + 3;
            int rssi = ev[0] == 0x02 ? 0 : (signed char)(ev[0] == 0x22 ? r[13] : r[13]);
            ULONG cod = ev[0] == 0x02 ? (r[9] | (r[10] << 8) | (r[11] << 16)) : (r[8] | (r[9] << 8) | (r[10] << 16));
            results++;
            Log("inquiry result #%lu: event %02x class %06lx rssi %d", (unsigned long)results, ev[0], (unsigned long)cod, rssi);
            continue;
        }
        HexLog("inquiry event", ev, n > 32 ? 32 : n);
    }
    Log("inquiry: no complete event, %lu results", (unsigned long)results);
}

static VOID BtThread(PVOID ctx)
{
    static const UCHAR edl_ver[] = { 0x19 };
    UCHAR ev[64], buf[64];
    int ok = 1, i;
    ULONG ios;

    UNREFERENCED_PARAMETER(ctx);
    g_ldo_on = 1;
    ok &= Ldo(9, 1, 1800000) == 0;
    ok &= Ldo(16, 1, 1704000) == 0;
    ok &= Ldo(17, 1, 1152000) == 0;
    ok &= Ldo(23, 1, 3200000) == 0;
    Log("ldos %s", ok ? "on (acked)" : "FAILED");
    SleepMs(20);
    if (ClocksOn()) {
        Log("se3 clocks failed");
        goto out;
    }
    if (UartInit())
        goto out;
    g_uart_ok = 1;
    for (i = 0; i < 4; i++)
        Log("gpio%d after: cfg %03lx io %lx", 8 + i, (unsigned long)Rd(g_pin[i], 0), (unsigned long)Rd(g_pin[i], 4));

    // hci_qca: off pulse at 2400, then on pulse at 115200, 100 ms boot time
    SetBaud(2400);
    {
        UCHAR b = 0xC0;
        UartTx(&b, 1);
    }
    SleepMs(50);
    SetBaud(115200);
    RxStart();
    {
        UCHAR b = 0xFC;
        UartTx(&b, 1);
    }
    RxCollect(buf, sizeof(buf), 300, "after on pulse");
    ios = Rd(g_se, SE_GENI_IOS);
    Log("geni ios %08lx: CTS %s", (unsigned long)ios, (ios & 2) ? "deasserted" : "asserted");
    if (!(ios & 2)) {
        Wr(g_se, SE_UART_TX_TRANS_CFG, 0);                 // obey CTS from now on (download is not acked per segment)
        Log("tx flow control on (CTS)");
    }
    Cmd(0xfc00, edl_ver, 1, "EDL patch version", ev, sizeof(ev), 1000);
    if (g_stop || Download(L"\\??\\C:\\j606f\\drv\\crbtfw21.tlv", "rampatch", 0))
        goto out;
    SleepMs(10);
    if (g_stop || Download(L"\\??\\C:\\j606f\\drv\\crnv21.bin", "nvm", 1))
        goto out;
    if (Cmd(0x0c03, NULL, 0, "HCI_Reset", NULL, 0, 2000) != 0)
        goto out;
    Cmd(0x1001, NULL, 0, "Read_Local_Version", NULL, 0, 1000);
    Cmd(0xfc00, edl_ver, 1, "EDL patch version", NULL, 0, 1000);
    {
        UCHAR e[64];
        // the address itself is not logged (private); only whether it is all-zero / the same as the chip default pattern
        if (Cmd(0x1009, NULL, 0, "!Read_BD_ADDR", e, sizeof(e), 1000) == 0)
            Log("Read_BD_ADDR ok: %s, oui-low byte %02x", (e[6] | e[7] | e[8] | e[9] | e[10] | e[11]) ? "non-zero" : "ZERO", e[11]);
    }
    Cmd(0x1003, NULL, 0, "Read_Local_Features", NULL, 0, 1000);
    Cmd(0x1005, NULL, 0, "Read_Buffer_Size", NULL, 0, 1000);
    Inquiry();
out:
    Log("thread done: se3 status %08lx, rx lost %lu junk %lu", (unsigned long)Rd(g_se, SE_GENI_STATUS), (unsigned long)g_lost,
        (unsigned long)g_junk);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID BtUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
    if (g_thread) {
        g_stop = 1;
        KeWaitForSingleObject(g_thread, 0, 0, FALSE, NULL);
        ObfDereferenceObject(g_thread);
        g_thread = NULL;
    }
    if (g_uart_ok) {
        RxStop();
        SetBaud(2400);
        {
            UCHAR off = 0xC0;
            UartTx(&off, 1);
        }
        Log("off pulse sent");
    }
    LdosOff();
    TxCmd(CMD_CLOSE, g_lcid, 0);
    WaitFor((volatile int *)&g_close_ack, 1000);
    Log("=== unload");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS st;
    HANDLE h;
    int i;

    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = BtUnload;
    Log("=== j606f btuart start (firmware download)");
    g_gcc = Map(GCC_PA, GCC_SIZE);
    g_se = Map(SE3_PA, SE_SIZE);
    for (i = 0; i < 4; i++)
        g_pin[i] = Map(TLMM_EAST + 0x1000 * (8 + i), 0x1000);
    if (!g_gcc || !g_se || !g_pin[0] || !g_pin[1] || !g_pin[2] || !g_pin[3])
        return STATUS_SUCCESS;
    if (GlinkOpen()) {
        Log("rpm: glink open failed");
        return STATUS_SUCCESS;
    }
    st = PsCreateSystemThread(&h, 0x1FFFFF, NULL, NULL, NULL, BtThread, NULL);
    if (NT_SUCCESS(st)) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(h, 0x1FFFFF, NULL, 0, &g_thread, NULL)))
            g_thread = NULL;
        ZwClose(h);
    }
    Log("bt thread %08lx", (unsigned long)st);
    return STATUS_SUCCESS;
}
#endif // BTUART_NO_ENTRY
