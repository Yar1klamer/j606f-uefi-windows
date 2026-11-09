// J606F USB host step 2: QUSB2 HS PHY init from Windows (included in btbus.sys: it already owns the RPM GLINK channel for LDO votes).
// In Windows URS0 (dwc3 host, xHCI) is up and usbc.sys gives VBUS + CC (run_35), but no device is seen: the HS PHY was never initialized
// (UEFI runs USB without it). Sequence = Linux phy-qcom-qusb2.c qusb2_phy_init() with sm6115_phy_cfg (msm8996 register layout),
// tuning values from the stock Lenovo DT (qcom,qusb-phy-init-seq of this tablet).
//  - LDOs (j606f DT): vdd pm6125 l4 0.925 V, vdda-pll l12 1.8 V, vdda-phy-dpdm l15 3.104 V (stock DT: vdd 0.925..0.97 V).
//  - GCC: GCC_AHB2PHY_USB_CLK 0x1d008 (cfg_ahb), reset GCC_QUSB2PHY_PRIM_BCR 0x1c000. Ref clock: single-ended scheme (CLK_REF_SEL).
//  - One-shot flag C:\j606f\drv\usbphy.arm (deleted when used). Writes only the PHY block 0x01613000 and the two GCC registers.
//  - The xHCI (0x04e00000) is only read: PORTSC of both ports before/after and every 30 s with the stats line.
#define QUSB2_PA            0x01613000
#define XHCI_PA             0x04e00000
#define GCC_AHB2PHY_USB     0x1d008
#define GCC_QUSB2PHY_BCR    0x1c000
#define Q_PLL_TEST          0x04
#define Q_PLL_STATUS        0x38
#define Q_PORT_POWERDOWN    0xb4

static volatile UCHAR *g_qphy, *g_xhci;
static int g_usbphy_tried, g_usbphy_ok;

static void XhciPorts(const char *tag)
{
    ULONG cap, op, n, i;

    ULONG gdsc = Rd(g_gcc, 0x1a004), cbcr = Rd(g_gcc, 0x1a010);

    if (!(gdsc & 0x80000000) || (cbcr & 0x80000000)) {      // an unclocked / powered-down dwc3 read could hang the bus
        Log("usb %s: xhci not read (gdsc %08lx master cbcr %08lx)", tag, (unsigned long)gdsc, (unsigned long)cbcr);
        return;
    }
    if (!g_xhci)
        g_xhci = Map(XHCI_PA, 0x1000);
    if (!g_xhci)
        return;
    cap = Rd(g_xhci, 0) & 0xff;
    n = (Rd(g_xhci, 4) >> 24) & 0xff;       // HCSPARAMS1 MaxPorts
    op = cap;
    if (n > 4)
        n = 4;
    for (i = 0; i < n; i++)
        Log("usb %s: xhci usbsts %08lx port%lu portsc %08lx", tag, (unsigned long)Rd(g_xhci, op + 4), (unsigned long)(i + 1),
            (unsigned long)Rd(g_xhci, op + 0x400 + 0x10 * i));
}

static void UsbPhyInit(void)
{
    static const UCHAR seq[][2] = {          // { offset, value } stock qcom,qusb-phy-init-seq
        { 0x80, 0xf8 }, { 0x84, 0x73 }, { 0x88, 0x93 }, { 0x8c, 0xc7 }, { 0x90, 0x11 }, { 0x08, 0x30 }, { 0x0c, 0x79 }, { 0x10, 0x21 },
        { 0x9c, 0x14 }, { 0x1c, 0x9f }, { 0x18, 0x00 },
    };
    ULONG v, i;
    int ok = 1;

    g_qphy = Map(QUSB2_PA, 0x1000);
    if (!g_qphy || !g_gcc) {
        Log("usbphy: map failed");
        return;
    }
    Log("usbphy: before: pll_test %08lx pll_status %08lx powerdown %08lx tune1 %08lx gcc ahb2phy %08lx bcr %08lx",
        (unsigned long)Rd(g_qphy, Q_PLL_TEST), (unsigned long)Rd(g_qphy, Q_PLL_STATUS), (unsigned long)Rd(g_qphy, Q_PORT_POWERDOWN),
        (unsigned long)Rd(g_qphy, 0x80), (unsigned long)Rd(g_gcc, GCC_AHB2PHY_USB), (unsigned long)Rd(g_gcc, GCC_QUSB2PHY_BCR));
    XhciPorts("before");
    ok &= Ldo(4, 1, 925000) == 0;
    ok &= Ldo(12, 1, 1800000) == 0;
    ok &= Ldo(15, 1, 3104000) == 0;
    Log("usbphy: ldo l4/l12/l15 %s", ok ? "on (acked)" : "FAILED");
    if (!ok)
        return;
    KeStallExecutionProcessor(200);
    Wr(g_gcc, GCC_AHB2PHY_USB, Rd(g_gcc, GCC_AHB2PHY_USB) | 1);
    for (i = 0; i < 100 && (Rd(g_gcc, GCC_AHB2PHY_USB) & 0x80000000); i++)
        KeStallExecutionProcessor(10);
    Wr(g_gcc, GCC_QUSB2PHY_BCR, Rd(g_gcc, GCC_QUSB2PHY_BCR) | 1);
    KeStallExecutionProcessor(150);
    Wr(g_gcc, GCC_QUSB2PHY_BCR, Rd(g_gcc, GCC_QUSB2PHY_BCR) & ~1u);
    KeStallExecutionProcessor(50);
    Wr(g_qphy, Q_PORT_POWERDOWN, Rd(g_qphy, Q_PORT_POWERDOWN) | 0x23);     // CLAMP_N_EN | FREEZIO_N | POWER_DOWN
    v = Rd(g_qphy, Q_PLL_TEST);
    for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        Wr(g_qphy, seq[i][0], seq[i][1]);
    Wr(g_qphy, Q_PORT_POWERDOWN, Rd(g_qphy, Q_PORT_POWERDOWN) & ~1u);      // enable
    KeStallExecutionProcessor(160);
    Wr(g_qphy, Q_PLL_TEST, v | 0x80);                                      // CLK_REF_SEL: single-ended ref clock (sm6115 default)
    (void)Rd(g_qphy, Q_PLL_TEST);
    KeStallExecutionProcessor(110);
    v = Rd(g_qphy, Q_PLL_STATUS);
    g_usbphy_ok = (v & 0x20) != 0;
    Log("usbphy: after: pll_status %08lx (%s) pll_test %08lx powerdown %08lx tune1..5 %02lx %02lx %02lx %02lx %02lx ahb2phy %08lx", (unsigned long)v,
        g_usbphy_ok ? "PLL LOCKED" : "no lock", (unsigned long)Rd(g_qphy, Q_PLL_TEST), (unsigned long)Rd(g_qphy, Q_PORT_POWERDOWN),
        (unsigned long)Rd(g_qphy, 0x80), (unsigned long)Rd(g_qphy, 0x84), (unsigned long)Rd(g_qphy, 0x88), (unsigned long)Rd(g_qphy, 0x8c),
        (unsigned long)Rd(g_qphy, 0x90), (unsigned long)Rd(g_gcc, GCC_AHB2PHY_USB));
    SleepMs(200);
    XhciPorts("after");
}
