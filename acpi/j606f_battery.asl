/*
 * J606F: battery for Windows through the inbox Control Method Battery driver (cmbatt.sys) — DRAFT, not in the UEFI yet.
 * The methods read the mm8013 fuel gauge (I2C0 = QUP0 SE0, address 0x55) by driving the GENI I2C controller over MMIO,
 * the same way as windrv/probe/probe.c ("j606fi2c" mode). Test that driver on the device first; only then build this in.
 * Sources: linux-sm61x5 gcc-sm6115.c, pinctrl-sm6115.c, sm6115.dtsi, qcom-geni-se.c, i2c-qcom-geni.c, mm8013.c.
 * Assumption to check: design voltage 3870 mV (only used by Windows to convert mAh to mWh).
 */
DefinitionBlock ("BAT.aml", "SSDT", 2, "LENOVO", "J606FBAT", 1)
{
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, Zero)
            Name (_PCL, Package () { \_SB })

            // GCC: APCS branch enable vote (0x7900c) and the QUP wrapper 0 branches / SE0 RCGR (0x1f0xx)
            OperationRegion (GVOT, SystemMemory, 0x0147900C, 0x04)
            Field (GVOT, DWordAcc, NoLock, Preserve) { VOTE, 32 }
            OperationRegion (GQUP, SystemMemory, 0x0141F000, 0x160)
            Field (GQUP, DWordAcc, NoLock, Preserve)
            {
                Offset (0x04), MAHB, 32, SAHB, 32, CORE, 32,
                Offset (0x14), C2XC, 32,
                Offset (0x144), S0CB, 32, RCMD, 32, RCFG, 32
            }
            // TLMM west tile: gpio0 / gpio1 config (function bits 5:2, pull bits 1:0)
            OperationRegion (TLG0, SystemMemory, 0x00500000, 0x04)
            Field (TLG0, DWordAcc, NoLock, Preserve) { G0CF, 32 }
            OperationRegion (TLG1, SystemMemory, 0x00501000, 0x04)
            Field (TLG1, DWordAcc, NoLock, Preserve) { G1CF, 32 }
            OperationRegion (QWRP, SystemMemory, 0x04AC0000, 0x08)
            Field (QWRP, DWordAcc, NoLock, Preserve) { Offset (0x04), QHWV, 32 }
            // GENI SE0
            OperationRegion (SEA, SystemMemory, 0x04A80000, 0x920)
            Field (SEA, DWordAcc, NoLock, Preserve)
            {
                Offset (0x20), FDEF, 32, OCTL, 32, CGCC, 32,
                Offset (0x40), GSTA, 32,
                Offset (0x48), MCLK, 32,
                Offset (0x64), IFDS, 32, FWRV, 32,
                Offset (0x7C), CSEL, 32,
                Offset (0x254), BGRN, 32, DMAE, 32, TPK0, 32, TPK1, 32,
                Offset (0x26C), TXTL, 32, RXTL, 32,
                Offset (0x278), SCLC, 32,
                Offset (0x284), RPK0, 32, RPK1, 32,
                Offset (0x600), MCMD, 32, MCTL, 32,
                Offset (0x610), MIST, 32, MIEN, 32, MICL, 32,
                Offset (0x648), SICL, 32,
                Offset (0x700), TXFF, 32,
                Offset (0x780), RXFF, 32,
                Offset (0x800), TXFS, 32, RXFS, 32,
                Offset (0x80C), TXWM, 32, RXWM, 32, RFRW, 32
            }
            OperationRegion (SEB, SystemMemory, 0x04A80C00, 0x240)
            Field (SEB, DWordAcc, NoLock, Preserve)
            {
                Offset (0x44), DTXC, 32,
                Offset (0x144), DRXC, 32,
                Offset (0x218), GSIE, 32, SIEN, 32,
                Offset (0x224), HWP0, 32,
                Offset (0x230), DGEN, 32
            }

            Name (STAT, Zero)        // 0 = not set up yet, 1 = ready, 2 = failed (stays failed)
            Name (ERRM, 0x342E)      // M_IRQ errors: overrun, illegal cmd, failure, abort, GP1 NACK, GP3 bus, GP4 arbitration

            // Wait for any bit of Arg0 (or an error) in M_IRQ_STATUS, at most ~20 ms. Returns the last status.
            Method (WIRQ, 1, Serialized)
            {
                Local1 = Zero
                While (Local1 < 400)
                {
                    Local0 = MIST
                    If (Local0 & (Arg0 | ERRM))
                    {
                        Return (Local0)
                    }
                    Stall (50)
                    Local1++
                }
                Return (Local0)
            }

            Method (ABRT, 0, Serialized)
            {
                MCTL = 0x02          // M_GENI_CMD_ABORT
                WIRQ (0x20)
                MICL = 0xFFFFFFFF
            }

            // Clocks, pins and SE0 set up as Linux i2c-qcom-geni (FIFO mode, 8-bit packing, 100 kHz from XO 19.2 MHz).
            Method (INIT, 0, Serialized)
            {
                If (STAT != Zero)
                {
                    Return (STAT == One)
                }
                STAT = 2
                VOTE |= 0x07C0       // m_ahb, s_ahb, core, core_2x, s0
                Local0 = Zero
                While (((MAHB | SAHB | CORE | S0CB) & 0x80000000) && (Local0 < 100))
                {
                    Sleep (1)
                    Local0++
                }
                If (Local0 >= 100)
                {
                    Return (Zero)
                }
                If (RCFG & 0x371F)   // source XO, no divider, no MND
                {
                    RCFG &= 0xFFFFC8E0
                    RCMD |= One
                    Local0 = Zero
                    While ((RCMD & One) && (Local0 < 100))
                    {
                        Sleep (1)
                        Local0++
                    }
                }
                If ((G0CF & 0x3F) != 0x07)   // function qup0, pull-up
                {
                    G0CF = ((G0CF & 0xFFFFFFC0) | 0x07)
                }
                If ((G1CF & 0x3F) != 0x07)
                {
                    G1CF = ((G1CF & 0xFFFFFFC0) | 0x07)
                }
                If (((FWRV >> 8) & 0xFF) != 0x03)   // SE firmware must be I2C (loaded by XBL)
                {
                    Return (Zero)
                }
                If (IFDS & One)                    // FIFO disabled: GPI DMA only
                {
                    Return (Zero)
                }
                Local1 = QHWV
                Local2 = (Local1 >> 28)
                If ((Local2 > 3) || ((Local2 == 3) && (((Local1 >> 16) & 0x0FFF) >= 10)))
                {
                    Local3 = ((HWP0 >> 16) & 0xFF)
                }
                Else
                {
                    Local3 = ((HWP0 >> 16) & 0x3F)
                }
                If (Local3 < 2)
                {
                    Local3 = 16
                }
                GSIE = Zero
                MICL = 0xFFFFFFFF
                SICL = 0xFFFFFFFF
                DTXC = 0xFFFFFFFF
                DRXC = 0xFFFFFFFF
                SIEN = 0xFFFFFFFF
                CGCC |= 0x7F
                DGEN |= 0x0F
                OCTL = 0x7F
                FDEF = One
                SIEN |= 0x0F
                DMAE &= 0xFFFFFFFE
                GSIE = Zero
                RXWM = (Local3 - 1)
                RFRW = Local3
                MIEN |= 0x7FC0007F      // common errors + CMD_DONE + TX/RX watermark + RX_LAST (as Linux)
                TPK0 = 0x0007F8FE
                TPK1 = 0x000FFEFE
                RPK0 = 0x0007F8FE
                RPK1 = 0x000FFEFE
                BGRN = Zero
                CSEL = Zero
                MCLK = 0x71             // clk_div 7, SER_CLK_EN
                SCLC = 0x00A0301A       // t_high 10, t_low 12, t_cycle 26
                STAT = One
                Return (One)
            }

            // SMBus read word from mm8013: write the register with a stretched stop, then read 2 bytes. Ones on error.
            Method (RDW, 1, Serialized)
            {
                If (!INIT ())
                {
                    Return (Ones)
                }
                If (GSTA & One)
                {
                    ABRT ()
                    Return (Ones)
                }
                MICL = 0xFFFFFFFF
                TXTL = One
                MCMD = 0x0800AA04       // I2C_WRITE, addr 0x55, STOP_STRETCH
                TXWM = One
                Local0 = WIRQ (0x40000000)
                If (!(Local0 & 0x40000000) || (Local0 & ERRM))
                {
                    TXWM = Zero
                    ABRT ()
                    Return (Ones)
                }
                TXFF = Arg0
                TXWM = Zero
                Local0 = WIRQ (One)
                MICL = Local0
                If (!(Local0 & One) || (Local0 & ERRM))
                {
                    If (!(Local0 & One))
                    {
                        ABRT ()
                    }
                    Return (Ones)
                }
                RXTL = 0x02
                MCMD = 0x1000AA00       // I2C_READ, addr 0x55
                Local0 = WIRQ (One)
                Local1 = RXFS
                If ((Local0 & ERRM) || !(Local1 & 0x01FFFFFF))
                {
                    MICL = 0xFFFFFFFF
                    If (!(Local0 & One))
                    {
                        ABRT ()
                    }
                    Return (Ones)
                }
                Local2 = (RXFF & 0xFFFF)
                If (!(Local0 & One))
                {
                    WIRQ (One)
                }
                MICL = 0xFFFFFFFF
                Return (Local2)
            }

            Method (_STA, 0, Serialized)
            {
                If (RDW (0x06) == Ones)    // temperature: no answer = no gauge
                {
                    Return (Zero)
                }
                Return (0x1F)
            }

            Name (BIXP, Package (0x14)
            {
                Zero,           // revision
                One,            // power unit: mA / mAh
                7518,           // design capacity (0x3C)
                7518,           // last full charge capacity (0x0E)
                One,            // rechargeable
                3870,           // design voltage mV (assumption)
                376,            // warning capacity
                150,            // low capacity
                Zero,           // cycle count (0x2A)
                95000,          // accuracy 95 %
                0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                One, One,       // granularity
                "J606F",
                "",
                "LiP",
                "Lenovo"
            })

            Method (_BIX, 0, Serialized)
            {
                Local0 = RDW (0x3C)
                If (Local0 != Ones)
                {
                    BIXP [0x02] = Local0
                }
                Local1 = RDW (0x0E)
                If (Local1 != Ones)
                {
                    BIXP [0x03] = Local1
                    BIXP [0x06] = (Local1 / 20)
                    BIXP [0x07] = (Local1 / 50)
                }
                Local2 = RDW (0x2A)
                If (Local2 != Ones)
                {
                    BIXP [0x08] = Local2
                }
                Return (BIXP)
            }

            Name (BSTP, Package (0x04) { Zero, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF })

            Method (_BST, 0, Serialized)
            {
                Local0 = RDW (0x0A)     // flags: bit0 DSG, bit8 CHG, bit9 FC
                Local1 = RDW (0x14)     // average current, s16
                Local2 = RDW (0x0C)     // remaining capacity mAh (Linux: charge_now)
                Local3 = RDW (0x08)     // voltage mV
                Local4 = Zero
                If (Local0 != Ones)
                {
                    If (Local0 & One)
                    {
                        Local4 = One
                    }
                    ElseIf (Local0 & 0x0100)
                    {
                        Local4 = 0x02
                    }
                }
                If (Local1 == Ones)
                {
                    Local1 = 0xFFFFFFFF
                }
                ElseIf (Local1 & 0x8000)
                {
                    Local1 = (0x00010000 - Local1)
                }
                If (Local2 == Ones)
                {
                    Local2 = 0xFFFFFFFF
                }
                If (Local3 == Ones)
                {
                    Local3 = 0xFFFFFFFF
                }
                BSTP [Zero] = Local4
                BSTP [One] = Local1
                BSTP [0x02] = Local2
                BSTP [0x03] = Local3
                Return (BSTP)
            }
        }

        Device (ADP0)
        {
            Name (_HID, "ACPI0003")
            Name (_PCL, Package () { \_SB })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }

            // Without SPMI (PMI632 USBIN status) this is an estimate: the gauge says charging or full.
            Method (_PSR, 0, Serialized)
            {
                Local0 = \_SB.BAT0.RDW (0x0A)
                If ((Local0 != Ones) && (Local0 & 0x0300))
                {
                    Return (One)
                }
                Return (Zero)
            }
        }
    }
}
