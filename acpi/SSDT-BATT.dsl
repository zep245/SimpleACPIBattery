/*
 * SSDT-BATT (Xiaomi Tiger Lake laptop) - macOS battery.
 * The firmware fills the battery's model / serial / type / maker (_BIF entries 9-12, _BIX entries 16-19) from 256-bit
 * memory fields (B1DN, B1SN, B1MN in SSDT "HQNV"), so they arrive as 32-byte BUFFERS where ACPI requires STRINGS.
 * macOS's battery driver rejects the package and publishes no battery. This wraps the original methods (renamed
 * _BIF -> XBIF and _BIX -> XBIX by two config.plist ACPI patches) and turns those entries into strings - on macOS only.
 */
DefinitionBlock ("", "SSDT", 2, "LOCAL", "BATT", 0x00001000)
{
    External (_SB_.PC00.LPCB.H_EC.BAT0, DeviceObj)
    External (_SB_.PC00.LPCB.H_EC.BAT0.XBIF, MethodObj)
    External (_SB_.PC00.LPCB.H_EC.BAT0.XBIX, MethodObj)

    Scope (\_SB.PC00.LPCB.H_EC.BAT0)
    {
        Method (BSTR, 2, NotSerialized)     // Arg0 = value, Arg1 = fallback string
        {
            Local0 = ObjectType (Arg0)
            If ((Local0 == 0x02))           // already a string
            {
                Return (Arg0)
            }
            If ((Local0 == 0x03))           // buffer -> string up to the first zero byte
            {
                Local1 = ToString (Arg0, Ones)
                If ((SizeOf (Local1) > Zero))
                {
                    Return (Local1)
                }
            }
            Return (Arg1)
        }

        Method (_BIF, 0, NotSerialized)  // _BIF: Battery Information
        {
            Local0 = XBIF ()
            If (_OSI ("Darwin"))
            {
                Local0 [0x09] = BSTR (DerefOf (Local0 [0x09]), "A5")
                Local0 [0x0A] = BSTR (DerefOf (Local0 [0x0A]), "123456789")
                Local0 [0x0B] = BSTR (DerefOf (Local0 [0x0B]), "LIon")
                Local0 [0x0C] = BSTR (DerefOf (Local0 [0x0C]), "TIMI")
            }
            Return (Local0)
        }

        Method (_BIX, 0, NotSerialized)  // _BIX: Battery Information Extended
        {
            Local0 = XBIX ()
            If (_OSI ("Darwin"))
            {
                Local0 [0x10] = BSTR (DerefOf (Local0 [0x10]), "A5")
                Local0 [0x11] = BSTR (DerefOf (Local0 [0x11]), "123456789")
                Local0 [0x12] = BSTR (DerefOf (Local0 [0x12]), "LIon")
                Local0 [0x13] = BSTR (DerefOf (Local0 [0x13]), "TIMI")
            }
            Return (Local0)
        }
    }
}
