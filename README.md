# SimpleACPIBattery

A small macOS kext for laptops with a plain ACPI battery (`PNP0C0A`) and AC adapter (`ACPI0003`), written for a
Xiaomi Tiger Lake laptop running macOS 15 with OpenCore. It reads the battery's ACPI methods (`_STA`, `_BIF`, `_BST`)
and the adapter's `_PSR` every 15 s and on ACPI notifications, and publishes them as an `IOPMPowerSource`, the object
macOS builds the menu-bar battery from. Every reading is logged (`SimpleACPIBattery:` in the kernel log).

## Files
- `SimpleACPIBattery.cpp`, `Info.plist`, `build.sh` - the kext. Build: GitHub Actions (`.github/workflows/build.yml`) or
  `bash build.sh <out dir>` on a Mac with Xcode. Artifact: `SimpleACPIBattery.kext` plus the `acpi/` folder.
- `acpi/SSDT-ECAV.aml` - required on the Xiaomi: the battery methods read the embedded controller only when
  `H_EC.ECAV` is 1, and the firmware sets it only for Windows (`_REV >= 2`). This sets it on macOS.
- `acpi/SSDT-BATT.aml` - turns the battery's name/serial fields (32-byte buffers) into strings. Needs the OpenCore
  ACPI patches `_BIF -> XBIF` and `_BIX -> XBIX` in BAT0.

## OpenCore
- ACPI/Add: `SSDT-ECAV.aml`, `SSDT-BATT.aml` (+ the two renames).
- Kernel/Add: `SimpleACPIBattery.kext` (after Lilu and VirtualSMC). Turn `SMCBatteryManager.kext` off.

## Check
    ioreg -rw0 -c IOPMPowerSource | grep -E "CurrentCapacity|MaxCapacity|IsCharging|ExternalConnected"
    log show --last boot --predicate 'eventMessage CONTAINS "SimpleACPIBattery"' --style compact | tail
`_STA 0x0B` in the log means the EC still reads 0 (SSDT-ECAV not loaded).
