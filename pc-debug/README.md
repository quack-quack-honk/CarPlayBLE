# PC Debug GUI for CarPlayBLE

A small Windows-friendly BLE GUI to drive the ESP32 display without the Android app.

- Uses Bleak to connect to the ESP32 GATT server (device name default: `Navigator`).
- Lets you write all navigation fields and pick the precise direction icon.

## Setup

1. Install Python 3.10+ (with Tkinter).
2. Open PowerShell in this folder and install deps:

```powershell
pip install -r requirements.txt
```

## Run

```powershell
python pc-debug/ble_debug_gui.py
```

- Enter the device name (Navigator) or its BLE address.
- Click Connect, then Send individual fields or Send All.

Notes:
- The UUIDs match the firmware in `CarPlay-*-` sketches. Adjust in the script if you’ve changed them.
- On Windows, Bleak requires Bluetooth enabled and the app allowed in privacy settings.
