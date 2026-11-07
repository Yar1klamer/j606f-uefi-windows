DefinitionBlock ("SSDT.aml", "SSDT", 2, "LENOVO", "J606F  ", 4)
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
    }    Scope (\_SB)
    {
        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, Zero)
            Name (_PCL, Package () { \_SB })
            Method (_STA, 0, NotSerialized)
            {
                Return (0x1F)
            }
            Name (BIXP, Package (0x14)
            {
                Zero, One, 7518, 7518, One, 3870, 376, 150, Zero, 95000,
                0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 75, 75, "J606F", "", "LiP", "Lenovo"
            })
            Method (_BIX, 0, NotSerialized)
            {
                Return (BIXP)
            }
            Name (BSTP, Package (0x04) { 0x02, 150, 3759, 3870 })     // charging, 150 mA, 50 %, 3870 mV — fixed test values
            Method (_BST, 0, NotSerialized)
            {
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
            Method (_PSR, 0, NotSerialized)
            {
                Return (One)
            }
        }
    }
}
