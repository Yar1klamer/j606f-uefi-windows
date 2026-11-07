/*
 * J606F battery/AC for cmbatt — compact form of j606f_battery_spmi.asl (same logic; sources and register notes there), merged into
 * the j606f SSDT (Silicium-ACPI/Platforms/Lenovo/j606f/SSDT.asl). Kept small on purpose: with FVMAIN at 0x522000 (v5, v5a) the device
 * resets before Windows, at 0x521000 (v4, v5b) it boots — so this AML must fit into what v4 leaves free (~0x330 bytes incl. v4 SSDT).
 * Dropped vs the full form: battery current (rate = unknown), _PCL, strings, APID cache.
 * PMI632 over SPMI (arbiter v5 observer channel, EE 0), read-only: QG 0x4880 voltage; SMB5 0x1006 charger state, 0x1310 bit4 USBIN.
 */
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            OperationRegion (SPCR, SystemMemory, 0x01C40900, 0x0800)
            Field (SPCR, DWordAcc, NoLock, Preserve) { AMAP, 0x4000 }

            // Arg0 = PPID (0x2xx), Arg1 = register; one byte, Ones on error
            Method (RD, 2, Serialized)
            {
                Local0 = AMAP
                Local1 = Zero
                While (Local1 < 0x0800)
                {
                    If ((DerefOf (Local0 [Local1 + One]) | ((DerefOf (Local0 [Local1 + 2]) & 0x0F) << 8)) == Arg0)
                    {
                        OperationRegion (OCH, SystemMemory, (0x03E00000 + (Local1 * 0x20)), 0x20)
                        Field (OCH, DWordAcc, NoLock, Preserve) { OCMD, 32, Offset (0x08), OSTA, 32, Offset (0x18), ORD0, 32 }
                        OCMD = (0x08000000 | (Arg1 << 4))
                        Local2 = 2000
                        While (Local2 && !(OSTA & One))
                        {
                            Stall (5)
                            Local2--
                        }
                        If ((OSTA & 0x0F) == One)
                        {
                            Return (ORD0 & 0xFF)
                        }
                        Break
                    }
                    Local1 += 4
                }
                Return (Ones)
            }

            Method (PLG, 0, Serialized)
            {
                Local0 = RD (0x0213, 0x10)
                If ((Local0 != Ones) && (Local0 & 0x10))
                {
                    Return (One)
                }
                Return (Zero)
            }

            // OCV mV, % (25 °C column of the stock QG profile)
            Name (OCVT, Package () { 4379, 100, 4149, 80, 3963, 60, 3870, 50, 3807, 30, 3708, 10, 3677, 5, 3501, 2, 3000, 0 })
            Name (BIXP, Package (0x14) { Zero, One, 7518, 7518, One, 3870, 376, 150, Zero, 95000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 75, 75, "", "", "", "" })
            Name (BSTP, Package (4) { Zero, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF })

            Method (_STA, 0, NotSerialized)
            {
                Return (0x1F)
            }

            Method (_BIX, 0, NotSerialized)
            {
                Return (BIXP)
            }

            Method (_BST, 0, Serialized)
            {
                Local0 = RD (0x0248, 0x80)          // QG S2_NORMAL_AVG_V, low byte
                Local1 = RD (0x0248, 0x81)
                // on a read error the last good values stay
                If ((Local0 | Local1) > 0xFF)
                {
                    Return (BSTP)
                }
                Local0 = ((((Local1 << 8) | Local0) * 194637) / 1000000)   // mV
                Local3 = Zero                       // %
                Local2 = Zero
                While (Local2 < 18)
                {
                    Local1 = DerefOf (OCVT [Local2])
                    If (Local0 >= Local1)
                    {
                        Local3 = DerefOf (OCVT [Local2 + One])
                        If (Local2)
                        {
                            Local3 += (((Local0 - Local1) * (DerefOf (OCVT [Local2 - One]) - Local3)) / (DerefOf (OCVT [Local2 - 2]) - Local1))
                        }
                        Break
                    }
                    Local2 += 2
                }
                Local4 = One                        // discharging
                If (PLG ())
                {
                    Local4 = Zero
                    Local5 = (RD (0x0210, 0x06) & 0x07)
                    If (Local5 <= 4)
                    {
                        Local4 = 2                  // charging
                    }
                    If ((Local5 >= 4) && (Local0 >= 4350))
                    {
                        Local3 = 100
                    }
                    ElseIf (Local3 > 99)
                    {
                        Local3 = 99
                    }
                }
                BSTP [Zero] = Local4
                BSTP [2] = ((Local3 * 7518) / 100)
                BSTP [3] = Local0
                Return (BSTP)
            }
        }

        Device (ADP0)
        {
            Name (_HID, "ACPI0003")
            Method (_PSR, 0, NotSerialized)
            {
                Return (\_SB.BAT0.PLG ())
            }
        }
    }
