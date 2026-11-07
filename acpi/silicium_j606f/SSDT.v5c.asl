DefinitionBlock ("SSDT.aml", "SSDT", 2, "LENOVO", "J606F  ", 5)
{
    External (\_SB.UFS0, DeviceObj)
    External (\_SB.SDC1, DeviceObj)
    External (\_SB.URS0, DeviceObj)

    Scope (\_SB)
    {
        Name (PSUB, "IDP06115")
    }

    Scope (\_SB.URS0)
    {
        Method (_STA, 0, NotSerialized) { Return (0x0F) }
    }

    Scope (\_SB.UFS0)
    {
        Method (_STA, 0, NotSerialized) { Return (0x0F) }
    }

    Scope (\_SB.SDC1)
    {
        Method (_STA, 0, NotSerialized) { Return (0x00) }
    }
    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, Zero)
            Name (_PCL, Package () { \_SB })

            // PMIC arbiter v5 core: version at 0, APID -> PPID map at 0x900 + 4 * apid (PPID in bits 19:8), 512 entries
            OperationRegion (SPCR, SystemMemory, 0x01C40000, 0x1100)
            Field (SPCR, DWordAcc, NoLock, Preserve)
            {
                AVER, 32,
                Offset (0x900), AMAP, 0x4000
            }

            Name (APCH, 0xFFFF)     // APID of PPID 0x210 (SMB5 CHGR)
            Name (APUS, 0xFFFF)     // APID of PPID 0x213 (SMB5 USBIN)
            Name (APQG, 0xFFFF)     // APID of PPID 0x248 (QG)

            // Arg0 = PPID; returns the APID or Ones
            Method (FAPI, 1, Serialized)
            {
                Local0 = AMAP
                Local1 = Zero
                While (Local1 < 0x0200)
                {
                    Local2 = (DerefOf (Local0 [(Local1 * 4) + 1]) | ((DerefOf (Local0 [(Local1 * 4) + 2]) & 0x0F) << 8))
                    If (Local2 == Arg0)
                    {
                        Return (Local1)
                    }
                    Local1++
                }
                Return (Ones)
            }

            // Arbiter usable (v5) and the three APIDs known. Returns One/Zero.
            Method (SPOK, 0, Serialized)
            {
                Local0 = AVER
                If ((Local0 < 0x50000000) || (Local0 >= 0x70000000))
                {
                    Return (Zero)
                }
                If (APQG == 0xFFFF)
                {
                    APCH = FAPI (0x0210)
                    APUS = FAPI (0x0213)
                    APQG = FAPI (0x0248)
                }
                If ((APCH == Ones) || (APUS == Ones) || (APQG == Ones))
                {
                    Return (Zero)
                }
                Return (One)
            }

            // Arg0 = APID, Arg1 = register (low byte of the address); one-byte read, Ones on error/timeout
            Method (SPRD, 2, Serialized)
            {
                OperationRegion (OCH, SystemMemory, (0x03E00000 + (Arg0 * 0x80)), 0x20)
                Field (OCH, DWordAcc, NoLock, Preserve)
                {
                    OCMD, 32,
                    Offset (0x08), OSTA, 32,
                    Offset (0x18), ORD0, 32
                }
                OCMD = (0x08000000 | ((Arg1 & 0xFF) << 4))     // PMIC_ARB_OP_EXT_READL, 1 byte
                Local0 = Zero
                Local1 = Zero
                While (Local0 < 2000)
                {
                    Local1 = OSTA
                    If (Local1 & One)
                    {
                        Break
                    }
                    Stall (5)
                    Local0++
                }
                If ((Local1 & 0x0F) != One)
                {
                    Return (Ones)
                }
                Return (ORD0 & 0xFF)
            }

            // Battery voltage in mV from QG S2_NORMAL_AVG_V, Ones on error
            Method (VBAT, 0, Serialized)
            {
                Local0 = SPRD (APQG, 0x80)
                Local1 = SPRD (APQG, 0x81)
                If ((Local0 == Ones) || (Local1 == Ones))
                {
                    Return (Ones)
                }
                Return ((((Local1 << 8) | Local0) * 194637) / 1000000)
            }

            // Battery current in mA (positive = discharge) from QG S2_NORMAL_AVG_I, s16 * 152588 nA (QG SUBTYPE 3 = IBAT 5A), Ones on error.
            // Windows run_08: -147 mA while charging from the Mac port; pmOS 18:07 (full, screen on): +61..248 mA.
            Method (IBAT, 0, Serialized)
            {
                Local0 = SPRD (APQG, 0x82)
                Local1 = SPRD (APQG, 0x83)
                If ((Local0 == Ones) || (Local1 == Ones))
                {
                    Return (Ones)
                }
                Local2 = ((Local1 << 8) | Local0)
                If (Local2 == 0x8000)
                {
                    Return (Ones)               // reset value: no measurement yet
                }
                If (Local2 & 0x8000)
                {
                    Return (Zero - (((0x00010000 - Local2) * 152588) / 1000000))
                }
                Return ((Local2 * 152588) / 1000000)
            }

            // OCV -> %, 25 °C column of the stock QG profile (mV, %), descending
            Name (OCVT, Package ()
            {
                4379, 100, 4265, 90, 4149, 80, 4047, 70, 3963, 60, 3870, 50, 3833, 40, 3807, 30,
                3773, 20, 3708, 10, 3699, 6, 3677, 5, 3635, 4, 3578, 3, 3501, 2, 3381, 1, 3000, 0
            })

            Method (SOCV, 1, Serialized)
            {
                If (Arg0 >= 4379)
                {
                    Return (100)
                }
                Local0 = 2
                While (Local0 < SizeOf (OCVT))
                {
                    Local1 = DerefOf (OCVT [Local0])
                    If (Arg0 >= Local1)
                    {
                        Local2 = DerefOf (OCVT [Local0 - 2])        // upper voltage
                        Local3 = DerefOf (OCVT [Local0 - 1])        // upper %
                        Local4 = DerefOf (OCVT [Local0 + 1])        // lower %
                        Return (Local4 + (((Arg0 - Local1) * (Local3 - Local4)) / (Local2 - Local1)))
                    }
                    Local0 += 2
                }
                Return (Zero)
            }

            Name (FCC, 7518)

            Method (_STA, 0, Serialized)
            {
                If (SPOK () == Zero)
                {
                    Return (Zero)
                }
                Local0 = SPRD (APQG, 0x08)      // QG STATUS1: bit0 battery present
                If ((Local0 != Ones) && ((Local0 & One) == Zero))
                {
                    Return (0x0F)               // slot present, no battery
                }
                Return (0x1F)
            }

            Name (BIXP, Package (0x14)
            {
                Zero,           // revision
                One,            // power unit: mA / mAh
                7518,           // design capacity
                7518,           // last full charge capacity
                One,            // rechargeable
                3870,           // design voltage mV (50 % point of the profile)
                376,            // warning capacity (5 %)
                150,            // low capacity (2 %)
                Zero,           // cycle count (unknown: mm8013 only)
                95000,          // accuracy
                0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
                75, 75,         // granularity (1 %)
                "J606F",
                "",
                "LiP",
                "Lenovo"
            })

            Method (_BIX, 0, NotSerialized)
            {
                Return (BIXP)
            }

            Name (BSTP, Package (0x04) { Zero, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF })

            Method (_BST, 0, Serialized)
            {
                BSTP [Zero] = Zero
                BSTP [One] = 0xFFFFFFFF
                BSTP [0x02] = 0xFFFFFFFF
                BSTP [0x03] = 0xFFFFFFFF
                If (SPOK () == Zero)
                {
                    Return (BSTP)
                }
                Local0 = VBAT ()
                Local1 = SPRD (APCH, 0x06)      // charger state, bits 2:0
                Local2 = SPRD (APUS, 0x10)      // USBIN INT_RT_STS, bit4 plug-in
                If (Local0 == Ones)
                {
                    Return (BSTP)
                }
                Local3 = SOCV (Local0)
                Local4 = One                    // discharging
                If ((Local2 != Ones) && (Local2 & 0x10))
                {
                    Local4 = Zero
                    If ((Local1 != Ones) && ((Local1 & 0x07) <= 4))
                    {
                        Local4 = 0x02           // charging
                    }
                    If ((Local1 != Ones) && ((Local1 & 0x07) >= 4) && (Local0 >= 4350))
                    {
                        Local3 = 100            // taper/terminated at the float voltage: full
                    }
                    ElseIf (Local3 > 99)
                    {
                        Local3 = 99             // voltage is raised by the charge current
                    }
                }
                Local5 = IBAT ()
                If (Local5 != Ones)
                {
                    If (Local5 & 0x8000000000000000)
                    {
                        Local5 = (Zero - Local5)
                    }
                    BSTP [One] = Local5         // rate, mA (absolute value; direction is in the state)
                }
                BSTP [Zero] = Local4
                BSTP [0x02] = ((Local3 * FCC) / 100)
                BSTP [0x03] = Local0
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

            Method (_PSR, 0, Serialized)
            {
                If (\_SB.BAT0.SPOK () == Zero)
                {
                    Return (One)
                }
                Local0 = \_SB.BAT0.SPRD (\_SB.BAT0.APUS, 0x10)
                If ((Local0 != Ones) && (Local0 & 0x10))
                {
                    Return (One)
                }
                Return (Zero)
            }
        }
    }
}
