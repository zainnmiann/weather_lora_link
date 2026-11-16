"""Resolve devices by USB serial number instead of port name (port names change with the USB socket)."""

from serial.tools import list_ports

# Known devices (CP2102N USB serial numbers), see DEVICES.md
KNOWN = {
    "weather": "90f34758ba91ed1187c8ccd13b20a988",
    "lora1": "f233d661ca67f011bbeffff6f3c77411",  # DevEui ..73:40:11:63
    "lora2": "7283499cc067f011ba030cf7f3c77411",  # DevEui ..73:40:02:C8
    "lora3": "b03fccd73b1af111ae640d77929fd6db",  # DevEui ..75:30:AA:8A
}


def resolve(spec):
    """'sn:<serial>' or 'dev:<name from KNOWN>' -> /dev/cu.* path; anything else is returned unchanged."""
    if spec.startswith("dev:"):
        spec = "sn:" + KNOWN[spec[4:]]
    if not spec.startswith("sn:"):
        return spec
    want = spec[3:]
    import sys
    for p in list_ports.comports():
        # on macOS use the /dev/cu.* node (not tty.*); on Linux the device is /dev/ttyUSB*
        if p.serial_number == want and (sys.platform != "darwin" or "/cu." in p.device):
            return p.device
    raise SystemExit(f"device with USB serial {want} is not connected")
