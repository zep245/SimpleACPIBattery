/*
 * SSDT-ECAV (Xiaomi Tiger Lake laptop) - lets the battery and charger methods read the embedded controller.
 * Every battery/AC method reads the EC through ECRD, which returns 0 unless H_EC.ECAV is 1. The firmware sets ECAV in
 * H_EC._INI only when _REV >= 2 (Windows), or in H_EC._REG for an EmbeddedControl region - and this EC has none (its
 * fields are SystemMemory at 0xFE0B0300), so macOS never calls it. With ECAV 0, BAT0._STA says "no battery" and
 * _BST/_BIF/_PSR read zeros: no battery driver can work. This sets ECAV (and ECTZ) on macOS only.
 */
DefinitionBlock ("", "SSDT", 2, "LOCAL", "ECAV", 0x00001000)
{
    External (_SB_.PC00.LPCB.H_EC, DeviceObj)
    External (_SB_.PC00.LPCB.H_EC.ECAV, IntObj)
    External (_SB_.PC00.LPCB.H_EC.ECTZ, IntObj)

    Scope (\_SB)
    {
        Device (ECAX)
        {
            Name (_HID, "ECAX0001")  // _HID: Hardware ID
            Method (_STA, 0, NotSerialized)  // _STA: Status
            {
                If (_OSI ("Darwin"))
                {
                    Return (0x0B)
                }
                Return (Zero)
            }
            Method (_INI, 0, NotSerialized)  // _INI: Initialize
            {
                If (_OSI ("Darwin"))
                {
                    If (CondRefOf (\_SB.PC00.LPCB.H_EC.ECAV))
                    {
                        \_SB.PC00.LPCB.H_EC.ECAV = One
                    }
                    If (CondRefOf (\_SB.PC00.LPCB.H_EC.ECTZ))
                    {
                        \_SB.PC00.LPCB.H_EC.ECTZ = One
                    }
                }
            }
        }
    }
}
