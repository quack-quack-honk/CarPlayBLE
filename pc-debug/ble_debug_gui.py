"""
PC BLE Debug GUI for CarPlayBLE (Waveshare/TTGO)

- Connects to the ESP32 GATT server (device name default: "Navigator").
- Lets you set each field that the Android app would normally send.
- Works on Windows using the Bleak library.

Usage (Windows PowerShell):
  pip install -r requirements.txt
  python pc-debug/ble_debug_gui.py
"""

import asyncio
import threading
from dataclasses import dataclass
from typing import Optional

try:
    import tkinter as tk
    from tkinter import ttk, messagebox
except Exception as e:
    raise SystemExit("Tkinter is required to run this GUI. Install Python with Tk support.")

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    raise SystemExit("Bleak is not installed. Run: pip install -r requirements.txt")


# GATT UUIDs (must match firmware)
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
DESTINATION_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
ETA_UUID = "ca83fac2-2438-4d14-a8ae-a01831c0cf0d"
DIRECTION_UUID = "dfc521a5-ce89-43bd-82a0-28a37f3a2b5a"
DIRECTION_DISTANCE_UUID = "0343ff39-994e-481b-9136-036dabc02a0b"
ETA_MINUTES_UUID = "563c187d-ff17-4a6a-8061-ca9b7b70b2b0"
DISTANCE_UUID = "8bf31540-eb0d-476c-b233-f514678d2afb"
DIRECTION_PRECISE_UUID = "a602346d-c2bb-4782-8ea7-196a11f85113"


DIR_CODES = [
    ("0", "ARRIVE"),
    ("1", "ARRIVE_LEFT"),
    ("2", "ARRIVE_RIGHT"),
    ("3", "CONTINUE_LEFT"),
    ("4", "CONTINUE_RETURN"),
    ("5", "CONTINUE_RIGHT"),
    ("6", "CONTINUE_SLIGHT_LEFT"),
    ("7", "CONTINUE_SLIGHT_RIGHT"),
    ("8", "CONTINUE_STRAIGHT"),
    ("9", "DEPART"),
    ("10", "FORK"),
    ("11", "POINTER"),
    ("12", "ROTATORY_EXIT"),
    ("13", "ROTATORY_EXIT_INVERTED"),
    ("14", "ROTATORY_LEFT"),
    ("15", "ROTATORY_LEFT_INVERTED"),
    ("16", "ROTATORY_RIGHT"),
    ("17", "ROTATORY_RIGHT_INVERTED"),
    ("18", "ROTATORY_SHARP_LEFT"),
    ("19", "ROTATORY_SHARP_LEFT_INVERTED"),
    ("20", "ROTATORY_SHARP_RIGHT"),
    ("21", "ROTATORY_SHARP_RIGHT_INVERTED"),
    ("22", "ROTATORY_SLIGHT_LEFT"),
    ("23", "ROTATORY_SLIGHT_LEFT_INVERTED"),
    ("24", "ROTATORY_SLIGHT_RIGHT"),
    ("25", "ROTATORY_SLIGHT_RIGHT_INVERTED"),
    ("26", "ROTATORY_STRAIGHT"),
    ("27", "ROTATORY_STRAIGHT_INVERTED"),
    ("28", "ROTATORY_TOTAL"),
    ("29", "ROTATORY_TOTAL_INVERTED"),
    ("30", "SHARP_LEFT"),
    ("31", "SHARP_RIGHT"),
    ("32", "SLIGHT_LEFT"),
    ("33", "SLIGHT_RIGHT"),
    ("34", "UNKNOWN"),
]


@dataclass
class AppState:
    loop: Optional[asyncio.AbstractEventLoop] = None
    client: Optional[BleakClient] = None
    connected: bool = False


class BleController:
    def __init__(self, state: AppState, log_cb):
        self.state = state
        self.log = log_cb

    async def scan_and_connect(self, name: str, address_hint: str = "", timeout: float = 8.0):
        self.log("Scanning for device…")
        device = None
        if address_hint:
            # Try by address first
            self.log(f"Trying address: {address_hint}")
            device = await BleakScanner.find_device_by_address(address_hint, timeout=timeout)

        if device is None:
            # Fallback: scan by name
            self.log(f"Scanning by name: {name}")
            devices = await BleakScanner.discover(timeout=timeout)
            for d in devices:
                if d.name == name:
                    device = d
                    break

        if device is None:
            raise RuntimeError("Device not found. Check it is advertising and within range.")

        self.log(f"Connecting to {device.name or device.address}…")
        client = BleakClient(device)
        await client.connect()
        # Optional: ensure services are loaded (handle Bleak API changes across versions)
        try:
            get_services = getattr(client, "get_services", None)
            if callable(get_services):
                await get_services()
            else:
                _ = client.services  # access property on newer Bleak
        except Exception:
            # Not critical; proceed without explicit service fetch
            pass
        self.state.client = client
        self.state.connected = True
        self.log("Connected.")

    async def disconnect(self):
        if self.state.client and self.state.client.is_connected:
            self.log("Disconnecting…")
            await self.state.client.disconnect()
        self.state.client = None
        self.state.connected = False
        self.log("Disconnected.")

    async def write_str(self, uuid: str, value: str):
        if not self.state.client or not self.state.client.is_connected:
            raise RuntimeError("Not connected")
        data = value.encode("utf-8")
        await self.state.client.write_gatt_char(uuid, data, response=False)
        self.log(f"Wrote {len(data)}B to {uuid}")


class DebugGUI:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title("CarPlayBLE Debugger")
        self.state = AppState()
        self._build_ui()

        # Create and start background asyncio loop
        try:
            asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())  # Windows-friendly
        except Exception:
            pass
        self.state.loop = asyncio.new_event_loop()
        self.ble = BleController(self.state, self._log)
        self.thread = threading.Thread(target=self._run_loop, daemon=True)
        self.thread.start()

    def _run_loop(self):
        asyncio.set_event_loop(self.state.loop)
        self.state.loop.run_forever()

    def _run_coro(self, coro):
        return asyncio.run_coroutine_threadsafe(coro, self.state.loop)

    # UI
    def _build_ui(self):
        frm = ttk.Frame(self.root, padding=8)
        frm.grid(sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        # Connection section
        row = 0
        ttk.Label(frm, text="Device Name:").grid(column=0, row=row, sticky="w")
        self.name_var = tk.StringVar(value="Navigator")
        ttk.Entry(frm, textvariable=self.name_var, width=24).grid(column=1, row=row, sticky="w")

        ttk.Label(frm, text="Address (optional):").grid(column=2, row=row, sticky="e")
        self.addr_var = tk.StringVar()
        ttk.Entry(frm, textvariable=self.addr_var, width=24).grid(column=3, row=row, sticky="w")

        self.connect_btn = ttk.Button(frm, text="Connect", command=self.on_connect)
        self.connect_btn.grid(column=4, row=row, padx=6)

        self.disconnect_btn = ttk.Button(frm, text="Disconnect", command=self.on_disconnect, state=tk.DISABLED)
        self.disconnect_btn.grid(column=5, row=row)

        row += 1
        ttk.Separator(frm).grid(column=0, columnspan=6, row=row, sticky="ew", pady=6)
        row += 1

        # Fields
        self._row_fields = {}
        def add_field(label, varname, default=""):
            nonlocal row
            ttk.Label(frm, text=label).grid(column=0, row=row, sticky="e")
            v = tk.StringVar(value=default)
            ent = ttk.Entry(frm, textvariable=v, width=48)
            ent.grid(column=1, row=row, columnspan=3, sticky="ew")
            btn = ttk.Button(frm, text="Send", command=lambda vn=varname, sv=v: self.on_send_one(vn, sv.get()))
            btn.grid(column=4, row=row, sticky="w")
            self._row_fields[varname] = v
            row += 1

        add_field("Destination", "destination", "Home")
        add_field("ETA", "eta", "12:34")
        add_field("Direction Text", "direction", "Turn right on Main St")
        add_field("Direction Distance", "direction_distance", "300 m")
        add_field("ETA Minutes", "eta_minutes", "10 min")
        add_field("Distance", "distance", "2.3 km")

        ttk.Label(frm, text="Direction Precise").grid(column=0, row=row, sticky="e")
        self.dir_code_var = tk.StringVar(value=DIR_CODES[0][0])
        self.dir_combo = ttk.Combobox(frm, textvariable=self.dir_code_var, width=30,
                                      values=[f"{c} - {n}" for c, n in DIR_CODES], state="readonly")
        self.dir_combo.grid(column=1, row=row, columnspan=3, sticky="ew")
        ttk.Button(frm, text="Send", command=self.on_send_dir_precise).grid(column=4, row=row, sticky="w")
        row += 1

        # Actions
        ttk.Button(frm, text="Send All", command=self.on_send_all).grid(column=4, row=row, pady=8, sticky="w")
        row += 1

        ttk.Separator(frm).grid(column=0, columnspan=6, row=row, sticky="ew", pady=6)
        row += 1

        # Log
        ttk.Label(frm, text="Log:").grid(column=0, row=row, sticky="nw")
        self.log_txt = tk.Text(frm, height=10, width=90)
        self.log_txt.grid(column=0, row=row, columnspan=6, sticky="nsew")
        frm.rowconfigure(row, weight=1)

    # Button handlers
    def on_connect(self):
        name = self.name_var.get().strip()
        addr = self.addr_var.get().strip()
        self.connect_btn.config(state=tk.DISABLED)
        def done(fut):
            exc = fut.exception()
            if exc:
                self._log(f"ERROR: {exc}")
                messagebox.showerror("Connect failed", str(exc))
                self.connect_btn.config(state=tk.NORMAL)
            else:
                self.connect_btn.config(state=tk.DISABLED)
                self.disconnect_btn.config(state=tk.NORMAL)
        self._run_coro(self.ble.scan_and_connect(name, addr)).add_done_callback(lambda f: self.root.after(0, done, f))

    def on_disconnect(self):
        self.disconnect_btn.config(state=tk.DISABLED)
        def done(_):
            self.connect_btn.config(state=tk.NORMAL)
        self._run_coro(self.ble.disconnect()).add_done_callback(lambda f: self.root.after(0, done, f))

    def on_send_one(self, field: str, value: str):
        uuid = {
            "destination": DESTINATION_UUID,
            "eta": ETA_UUID,
            "direction": DIRECTION_UUID,
            "direction_distance": DIRECTION_DISTANCE_UUID,
            "eta_minutes": ETA_MINUTES_UUID,
            "distance": DISTANCE_UUID,
        }[field]
        self._run_coro(self.ble.write_str(uuid, value))

    def on_send_dir_precise(self):
        code = self.dir_code_var.get().split(" ")[0]
        self._run_coro(self.ble.write_str(DIRECTION_PRECISE_UUID, code))

    def on_send_all(self):
        # Send in sequence
        async def send_all():
            await self.ble.write_str(DESTINATION_UUID, self._row_fields["destination"].get())
            await self.ble.write_str(ETA_UUID, self._row_fields["eta"].get())
            await self.ble.write_str(DIRECTION_UUID, self._row_fields["direction"].get())
            await self.ble.write_str(DIRECTION_DISTANCE_UUID, self._row_fields["direction_distance"].get())
            await self.ble.write_str(ETA_MINUTES_UUID, self._row_fields["eta_minutes"].get())
            await self.ble.write_str(DISTANCE_UUID, self._row_fields["distance"].get())
            code = self.dir_code_var.get().split(" ")[0]
            await self.ble.write_str(DIRECTION_PRECISE_UUID, code)
        self._run_coro(send_all())

    # Logging helper
    def _log(self, msg: str):
        self.log_txt.insert("end", msg + "\n")
        self.log_txt.see("end")


def main():
    root = tk.Tk()
    DebugGUI(root)
    root.mainloop()


if __name__ == "__main__":
    main()
