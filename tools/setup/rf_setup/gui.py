"""Setup window (Tkinter, included with Python on Windows and most Linux).

A step-by-step wizard over the same steps as the guided CLI (flow.py). Each
step's work runs on a background thread; the window stays responsive and
shows progress in the log at the bottom.
"""

from __future__ import annotations

import os
import queue
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import core, flow
from .core import SetupError

STEPS = ["Connect", "Firmware", "Wi-Fi", "Certificate", "Claim", "Name", "Network", "DNS", "Let's Encrypt", "Finish"]
PAD = {"padx": 10, "pady": 4}


class App(tk.Tk):
    def __init__(self, firmware_dir: str | None = None):
        super().__init__()
        self.title("RF Filter Controller Setup")
        self.geometry("860x620")
        self.minsize(760, 560)
        self.firmware_dir = firmware_dir
        self.events: "queue.Queue[tuple]" = queue.Queue()
        self.session = flow.Session(log=lambda m: self.events.put(("log", m)))
        self.step = 0
        self.busy = False
        self._build()
        self.show(0)
        self.after(100, self._pump)

    # -- layout -------------------------------------------------------------------
    def _build(self):
        style = ttk.Style(self)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        side = ttk.Frame(self, padding=(12, 12))
        side.pack(side="left", fill="y")
        ttk.Label(side, text="Setup", font=("Segoe UI", 13, "bold")).pack(anchor="w", pady=(0, 8))
        self.step_labels = []
        for i, name in enumerate(STEPS):
            lbl = ttk.Label(side, text=f"{i + 1}. {name}")
            lbl.pack(anchor="w", pady=2)
            self.step_labels.append(lbl)

        main = ttk.Frame(self, padding=(4, 12, 12, 8))
        main.pack(side="left", fill="both", expand=True)
        self.title_var = tk.StringVar()
        ttk.Label(main, textvariable=self.title_var, font=("Segoe UI", 14, "bold")).pack(anchor="w")
        self.body = ttk.Frame(main)
        self.body.pack(fill="both", expand=True, pady=(8, 8))

        self.progress = ttk.Progressbar(main, mode="indeterminate")
        self.progress.pack(fill="x")
        logf = ttk.Frame(main)
        logf.pack(fill="both", expand=False, pady=(6, 6))
        self.log = tk.Text(logf, height=9, wrap="word", state="disabled", font=("Consolas", 9))
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        self.log.configure(yscrollcommand=sb.set)
        self.log.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        nav = ttk.Frame(main)
        nav.pack(fill="x")
        self.next_btn = ttk.Button(nav, text="Next", command=self.next)
        self.next_btn.pack(side="right")
        ttk.Button(nav, text="Quit", command=self.destroy).pack(side="left")

    def _clear(self):
        for w in self.body.winfo_children():
            w.destroy()

    def text(self, message: str, wrap: int = 560):
        ttk.Label(self.body, text=message, wraplength=wrap, justify="left").pack(anchor="w", **PAD)

    def field(self, label: str, value: str = "", show: str = "") -> tk.StringVar:
        row = ttk.Frame(self.body)
        row.pack(fill="x", **PAD)
        ttk.Label(row, text=label, width=26).pack(side="left")
        var = tk.StringVar(value=value)
        ttk.Entry(row, textvariable=var, show=show).pack(side="left", fill="x", expand=True)
        return var

    def check(self, label: str, value: bool = False, command=None) -> tk.BooleanVar:
        var = tk.BooleanVar(value=value)
        ttk.Checkbutton(self.body, text=label, variable=var, command=command).pack(anchor="w", **PAD)
        return var

    # -- plumbing -------------------------------------------------------------------
    def _pump(self):
        try:
            while True:
                kind, *rest = self.events.get_nowait()
                if kind == "log":
                    self._append(rest[0])
                elif kind == "done":
                    self._finished(*rest)
        except queue.Empty:
            pass
        self.after(100, self._pump)

    def _append(self, message: str):
        self.log.configure(state="normal")
        self.log.insert("end", message + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def work(self, fn, then):
        """Run fn on a thread; call then(result) on success."""
        self.busy = True
        self.next_btn.configure(state="disabled")
        self.progress.start(12)

        def target():
            try:
                self.events.put(("done", True, fn(), then))
            except SetupError as exc:
                self.events.put(("done", False, str(exc), then))
            except Exception as exc:  # shown to the user rather than lost on a thread
                self.events.put(("done", False, f"Unexpected error: {exc!r}", then))
        threading.Thread(target=target, daemon=True).start()

    def _finished(self, ok, result, then):
        self.busy = False
        self.progress.stop()
        self.next_btn.configure(state="normal")
        if ok:
            then(result)
        else:
            self._append("Stopped: " + result)
            messagebox.showerror("Setup stopped", result + "\n\nFix the problem and press Next to try again.")

    def show(self, step: int):
        self.step = step
        for i, lbl in enumerate(self.step_labels):
            lbl.configure(font=("Segoe UI", 10, "bold" if i == step else "normal"),
                          foreground="" if i >= step else "gray40")
        self.title_var.set(STEPS[step])
        self._clear()
        self.next_btn.configure(text="Next", command=self.next)
        getattr(self, f"page_{step}")()

    def next(self):
        if not self.busy:
            getattr(self, f"do_{self.step}")()

    def advance(self, _result=None):
        self.show(self.step + 1)

    # -- 1. connect -----------------------------------------------------------------
    def page_0(self):
        self.text("Connect the board to this computer with a USB cable on its native USB port "
                  "(USB Serial/JTAG). Have your Wi-Fi network name and password ready.")
        self.ports = core.list_serial_ports()
        self.port_var = tk.StringVar()
        row = ttk.Frame(self.body)
        row.pack(fill="x", **PAD)
        ttk.Label(row, text="Serial port", width=26).pack(side="left")
        values = [f"{d}  {desc}" for d, desc in self.ports]
        box = ttk.Combobox(row, textvariable=self.port_var, values=values, state="readonly")
        box.pack(side="left", fill="x", expand=True)
        ttk.Button(row, text="Refresh", command=lambda: self.show(0)).pack(side="left", padx=4)
        if values:
            box.current(0)
        else:
            self.text("No serial ports found. Connect the USB cable, then press Refresh.")

    def do_0(self):
        if not self.port_var.get():
            messagebox.showwarning("Choose a port", "Choose the board's serial port.")
            return
        self.session.port = self.port_var.get().split()[0]
        self.advance()

    # -- 2. firmware ----------------------------------------------------------------
    def page_1(self):
        self.text("A new board needs the firmware written once over USB. A board that already runs it "
                  "doesn't; later updates go over the network. Writing keeps the board's settings, "
                  "Wi-Fi and ownership.")
        self.flash_var = self.check("Write the firmware to this board", False)
        self.text("Next then reads the board's state from its USB console.")
        self.next_btn.configure(text="Start")

    def do_1(self):
        flash = self.flash_var.get()
        if flash and not messagebox.askyesno("Write firmware?",
                                             f"Write the firmware to the board on {self.session.port}?"):
            return

        def job():
            if flash:
                self.session.flash(self.firmware_dir)
            return self.session.read_board()
        self.work(job, self.advance)

    # -- 3. Wi-Fi ---------------------------------------------------------------------
    def page_2(self):
        s = self.session
        if s.needs_wifi():
            self.text("The board needs your Wi-Fi network (2.4 GHz). The password is sent to the board "
                      "over USB and isn't shown or saved anywhere else.")
            self.wifi_var = None
        else:
            self.text(f"The board is on Wi-Fi at {s.info.ip}. Tick the box to move it to another network.")
            self.wifi_var = self.check("Change the Wi-Fi network", False)
        self.ssid_var = self.field("Network name (SSID)")
        self.wpw_var = self.field("Password", show="*")

    def do_2(self):
        if self.wifi_var is not None and not self.wifi_var.get():
            self.advance()
            return
        ssid, pw = self.ssid_var.get().strip(), self.wpw_var.get()
        if not ssid:
            messagebox.showwarning("Network name", "Enter the Wi-Fi network name.")
            return
        self.work(lambda: self.session.wifi(ssid, pw), self.advance)

    # -- 4. credentials -------------------------------------------------------------
    def page_3(self):
        b = self.session.info
        if b.provisioned:
            self.text("This board is already claimed. Choose the credentials folder of the CA it was "
                      "claimed with, and its password if it has one.")
            self.existing_var = None
        else:
            self.text("Management needs a client certificate from your management CA. Create a new one, "
                      "or use one you already have, such as the time server's credentials folder. Keep "
                      "the folder and password safe: if both are lost, the board's management cannot be "
                      "recovered remotely.")
            self.existing_var = self.check("Use an existing credentials folder", False,
                                           command=lambda: self._cred_mode())
        self.folder_var = self.field("Credentials folder",
                                     str(core.default_credentials_folder("rf-controller")))
        ttk.Button(self.body, text="Browse...", command=self._browse).pack(anchor="w", **PAD)
        self.pw_var = self.field("Password", show="*")
        self.pw2_var = self.field("Password again (new folder)", show="*")

    def _cred_mode(self):
        if self.existing_var is not None and self.existing_var.get():
            self.pw2_var.set("")

    def _existing(self) -> bool:
        return self.existing_var is None or self.existing_var.get()

    def _browse(self):
        d = filedialog.askdirectory(initialdir=os.path.expanduser("~"))
        if d:
            self.folder_var.set(d)

    def do_3(self):
        pw = self.pw_var.get()
        folder = self.folder_var.get().strip()
        if self._existing():
            self.work(lambda: self.session.existing_credentials(folder, pw), self.advance)
            return
        if pw != self.pw2_var.get():
            messagebox.showwarning("Passwords differ", "The two passwords do not match.")
            return
        self.work(lambda: self.session.new_credentials(folder, pw), self.advance)

    # -- 5. claim -------------------------------------------------------------------
    def page_4(self):
        b = self.session.info
        if b.provisioned:
            self.text(f"The board at {b.ip} is already claimed. Next connects to it with your certificate.")
            self.next_btn.configure(text="Connect")
        else:
            self.text(f"The board at {b.ip} is ready to be claimed. Afterwards it accepts management only "
                      "from your certificate. Its identity is checked against the fingerprint on its USB "
                      f"console ({b.fingerprint[:16] if b.fingerprint else '?'}...), so nothing is sent to "
                      "an impostor.")
            self.next_btn.configure(text="Claim")

    def do_4(self):
        self.work(self.session.claim, self.advance)

    # -- 6. identity ----------------------------------------------------------------
    def page_5(self):
        host, ntp0, ntp1 = self.session.current_identity()
        self.text("Name the board, and choose the NTP servers it sets its clock from. These apply when "
                  "the board restarts at the end of the next step.")
        self.host_var = self.field("Hostname", host)
        self.ntp0_var = self.field("NTP server 1", ntp0)
        self.ntp1_var = self.field("NTP server 2", ntp1)

    def do_5(self):
        args = (self.host_var.get().strip(), self.ntp0_var.get().strip(), self.ntp1_var.get().strip())
        self.work(lambda: self.session.identity(*args), self.advance)

    # -- 7. network -----------------------------------------------------------------
    def page_6(self):
        self.text("Either reserve the board's address on your router and keep DHCP, or set a static "
                  "address here. Next restarts the board to apply its name, NTP servers and network.")
        self.static_var = self.check("Set a static address on the board")
        self.addr_var = self.field("Address", self.session.board.host)
        self.mask_var = self.field("Netmask", "")
        self.gw_var = self.field("Gateway", "")
        self.dns_var = self.field("DNS server", "")

        def fill(net):
            self.static_var.set(not net["dhcp"])
            self.addr_var.set(net["address"]); self.mask_var.set(net["netmask"])
            self.gw_var.set(net["gateway"]); self.dns_var.set(net["dns"])
        self.work(self.session.current_network, fill)

    def do_6(self):
        static = self.static_var.get()
        vals = [v.get().strip() for v in (self.addr_var, self.mask_var, self.gw_var, self.dns_var)]
        if static and not messagebox.askyesno(
                "Apply static address?",
                f"Restart the board on {vals[0]}? If the settings are wrong it will be unreachable over the "
                "network until they are corrected."):
            return
        if static:
            self.work(lambda: self.session.network(True, *vals), self.advance)
        else:
            self.work(lambda: self.session.network(False), self.advance)

    # -- 8. DNS / Cloudflare ----------------------------------------------------------
    def page_7(self):
        fqdn, zone = self.session.current_fqdn()
        self.text("A Let's Encrypt certificate lets browsers trust the board by name. The board proves it "
                  "controls the name with a Cloudflare API token limited to Zone:Read and DNS:Edit on your "
                  "zone. The token is stored on the board. Untick to skip this and the next step.")
        self.cert_var = self.check("Set up a Let's Encrypt certificate", True)
        self.fqdn_var = self.field("Full DNS name", fqdn or f"{self.host_var.get().strip()}.example.com")
        self.zone_var = self.field("Cloudflare zone", zone)
        self.token_var = self.field("API token", show="*")
        self.dns_var = self.check(f"Create or update the DNS record for the name -> {self.session.board.host}", True)

    def do_7(self):
        if not self.cert_var.get():
            self.show(9)
            return
        fqdn, zone = self.fqdn_var.get().strip(), self.zone_var.get().strip()
        token, create = self.token_var.get().strip(), self.dns_var.get()
        if not token:
            messagebox.showwarning("Token needed", "Enter the Cloudflare API token.")
            return
        self.work(lambda: self.session.cloudflare(fqdn, token, zone, create), self.advance)

    # -- 9. Let's Encrypt ---------------------------------------------------------------
    def page_8(self):
        self.text(f"Next asks Let's Encrypt for a certificate for {self.session.record.fqdn}. It usually "
                  "takes 1 to 3 minutes. The board then switches HTTPS to it, and this tool checks it the "
                  "way a browser would.")
        self.staging_var = self.check("Try Let's Encrypt staging first (an untrusted test certificate)", False)
        self.next_btn.configure(text="Get certificate")

    def do_8(self):
        staging = self.staging_var.get()

        def job():
            if staging:
                self.session.certificate(production=False)
            return self.session.certificate(production=True)
        self.work(job, self.advance)

    # -- 10. finish -------------------------------------------------------------------
    def page_9(self):
        self.text("Almost done.")
        self.imp_var = (self.check("Install the administrator certificate in Windows for the browser", True)
                        if self.session.can_import() else None)
        self.next_btn.configure(text="Finish")

    def do_9(self):
        imp = bool(self.imp_var and self.imp_var.get())

        def done(notes):
            self._clear()
            self.title_var.set("Done: open the console in your browser")
            box = tk.Text(self.body, wrap="word", height=18, font=("Segoe UI", 10), relief="flat",
                          background=self.cget("background"))
            sb = ttk.Scrollbar(self.body, command=box.yview)
            box.configure(yscrollcommand=sb.set)
            box.tag_configure("h", font=("Segoe UI", 10, "bold"), spacing1=8)
            for n in notes:
                box.insert("end", n + "\n")
            for title, steps in self.session.browser_steps(imp):
                box.insert("end", title + "\n", "h")
                for i, step in enumerate(steps, 1):
                    box.insert("end", f"  {i}. {step}\n")
            box.configure(state="disabled")
            box.pack(side="left", fill="both", expand=True, padx=10)
            sb.pack(side="right", fill="y")
            self.next_btn.configure(text="Close", command=self.destroy)
        self.work(lambda: self.session.finish(imp), done)


def run(firmware_dir: str | None = None) -> int:
    App(firmware_dir).mainloop()
    return 0
