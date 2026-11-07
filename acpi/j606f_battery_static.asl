/*
 * J606F: bisect step for the BATS table — the same BAT0/ADP0 devices with constant values and NO operation regions.
 * If Windows boots with this one, the early reset with j606f_battery_spmi.asl comes from the SystemMemory regions / SPMI reads in AML.
 */
DefinitionBlock ("BATS.aml", "SSDT", 2, "LENOVO", "J606FBAT", 1)
{
    Scope (\_SB)
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
