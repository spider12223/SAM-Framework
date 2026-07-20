"""
S.A.M Framework Installer
-------------------------
A standalone Windows installer that drops the S.A.M-patched barony.exe (plus
typescript.js, the TypeScript compiler for .ts mod scripts) into the player's
Barony game folder, backing up their original first. Built with tkinter +
PyInstaller.

Where the payload comes from (see resolve_payload):
  * If the installer was built WITH a bundled payload (the old .spec passed the
    files via `datas`), it installs those — works fully offline.
  * Otherwise it DOWNLOADS the newest release assets from GitHub at install time.
    This is the default from v1.0.0 on: it means publishing a release updates every
    installer already out in the wild, so we never rebuild and re-upload an installer
    per version again. It also drops the build from ~18 MB to ~11.6 MB — not smaller,
    because PyInstaller's bundled Python + tkinter runtime is the bulk of it; only the
    payload's ~6.4 MB compressed footprint goes away. The real win is not rebuilding.

  python installer.py                    -> launch the GUI installer
  python installer.py --selftest         -> Steam/Barony auto-detection + payload source
  python installer.py --selftest-download-> exercise the real download + verification
"""

import os
import re
import sys
import queue
import shutil
import tempfile
import threading
import urllib.error
import urllib.request

# --------------------------------------------------------------------------- #
#  Constants
# --------------------------------------------------------------------------- #
APP_VERSION = "1.6.0"
APP_TITLE = f"S.A.M Framework Installer v{APP_VERSION}"
PAYLOAD_NAME = "sam_barony.exe"   # bundled S.A.M barony.exe (see --add-data)
TS_PAYLOAD_NAME = "typescript.js" # bundled TypeScript compiler for .ts mod scripts

# Where to fetch the payload when this installer was built WITHOUT a bundled copy.
# "/releases/latest/download/<asset>" is a permanent GitHub redirect to the newest
# release's asset of that name — no API call, so no token and no rate limit. That is
# the whole point: cutting a release updates every installer already in the wild, and
# we never rebuild/re-upload an 18 MB installer again. It only works while the asset
# names below stay stable across releases — do not rename them.
REPO_SLUG = "spider12223/SAM-Framework"
RELEASE_LATEST = "https://github.com/" + REPO_SLUG + "/releases/latest/download"
EXE_ASSET = "barony.exe"
TS_ASSET = "typescript.js"
MIN_EXE_BYTES = 4 * 1024 * 1024   # a real barony.exe is ~13 MB; anything tiny is an error page

# BSD 2-Clause requires this notice be reproduced when distributing the binary.
ATTRIBUTION = "Built on Barony (BSD 2-Clause)  © 2013-2020 Turning Wheel LLC"

# S.A.M palette (matches the Mod Builder GUI)
STONE   = "#1a1208"
WOOD    = "#2a1f0e"
WELL    = "#211808"
GOLD    = "#d4a84b"
GOLD_HI = "#f0cd7a"
GOLD_DIM= "#8a6d2e"
PARCH   = "#e8d5a3"
BORDER  = "#4a3617"
GREEN   = "#8fc76a"
RED     = "#e07a6a"
AMBER   = "#e0b46a"

FONT_FAMILY = "Palatino Linotype"   # ships with Windows; Tk substitutes if absent


# --------------------------------------------------------------------------- #
#  Detection + install logic (pure, GUI-independent — also used by --selftest)
# --------------------------------------------------------------------------- #
def resource_path(name):
    """Absolute path to a bundled resource, whether running frozen or from source."""
    if getattr(sys, "frozen", False):
        return os.path.join(sys._MEIPASS, name)
    here = os.path.dirname(os.path.abspath(__file__))
    payload = os.path.join(here, "payload", name)
    return payload if os.path.isfile(payload) else os.path.join(here, name)


def find_steam():
    """Steam install path from the registry, or None."""
    try:
        import winreg
    except ImportError:
        return None
    candidates = [
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
        (winreg.HKEY_CURRENT_USER,  r"SOFTWARE\Valve\Steam",             "InstallPath"),
        (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam",             "InstallPath"),
    ]
    for hive, key, value in candidates:
        try:
            with winreg.OpenKey(hive, key) as k:
                path, _ = winreg.QueryValueEx(k, value)
            if path and os.path.isdir(path):
                return path
        except OSError:
            continue
    return None


def steam_library_roots(steam_path):
    """Every Steam library folder (the main one + extra drives from the .vdf)."""
    roots = []
    if not steam_path:
        return roots
    roots.append(steam_path)
    vdf = os.path.join(steam_path, "steamapps", "libraryfolders.vdf")
    if os.path.isfile(vdf):
        try:
            with open(vdf, encoding="utf-8", errors="ignore") as f:
                text = f.read()
            for m in re.finditer(r'"path"\s*"([^"]+)"', text):
                p = m.group(1).replace("\\\\", "\\")
                if p not in roots and os.path.isdir(p):
                    roots.append(p)
        except OSError:
            pass
    return roots


def find_barony():
    """Full path to <library>\\steamapps\\common\\Barony\\barony.exe, or None."""
    for root in steam_library_roots(find_steam()):
        candidate = os.path.join(root, "steamapps", "common", "Barony", "barony.exe")
        if os.path.isfile(candidate):
            return candidate
    return None


def _download(url, dst, label, on_progress=None, lo=0.0, hi=1.0):
    """Stream `url` to `dst`, reporting progress into the [lo, hi] slice.
    Raises a player-readable RuntimeError on any network failure."""
    req = urllib.request.Request(url, headers={"User-Agent": "SAM-Framework-Installer/" + APP_VERSION})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp, open(dst, "wb") as out:
            total = 0
            try:
                total = int(resp.headers.get("Content-Length") or 0)
            except (TypeError, ValueError):
                total = 0
            done = 0
            while True:
                chunk = resp.read(256 * 1024)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                if on_progress:
                    frac = (done / total) if total else 0.0
                    mb = done / (1024 * 1024)
                    text = "Downloading %s… %.1f MB" % (label, mb)
                    if total:
                        text += " of %.1f MB" % (total / (1024 * 1024))
                    on_progress(lo + (hi - lo) * min(1.0, frac), text)
    except urllib.error.HTTPError as exc:
        raise RuntimeError(
            "Couldn't download %s from GitHub (HTTP %s).\n\n"
            "The release may still be publishing. Wait a minute and try again, or grab "
            "%s manually from:\n%s" % (label, exc.code, label, RELEASE_LATEST)
        ) from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise RuntimeError(
            "Couldn't reach GitHub to download %s.\n\n"
            "This installer fetches the latest S.A.M build at install time, so it needs "
            "an internet connection. Check your connection (or a firewall/proxy blocking "
            "it) and try again.\n\nDetails: %s" % (label, exc)
        ) from exc


def _verify_exe(path):
    """Make sure we downloaded a real Windows executable and not a truncated file or an
    HTML error page. This runs BEFORE we touch the player's game folder — writing a
    bogus barony.exe would leave them unable to launch."""
    size = os.path.getsize(path) if os.path.isfile(path) else 0
    if size < MIN_EXE_BYTES:
        raise RuntimeError(
            "The downloaded barony.exe is only %d bytes, which is far too small to be "
            "real — the download was probably interrupted or GitHub returned an error "
            "page. Nothing was changed; please try again." % size
        )
    with open(path, "rb") as f:
        if f.read(2) != b"MZ":
            raise RuntimeError(
                "The downloaded barony.exe isn't a Windows executable (missing the 'MZ' "
                "header). Nothing was changed; please try again."
            )


def resolve_payload(on_progress=None, lo=0.0, hi=1.0):
    """Return (sam_exe_path, typescript_path) to install from.

    If this installer was built with a bundled payload we use it (works offline). A
    lean build has no bundle, so we download the newest release assets instead — that
    is what lets a new S.A.M release reach existing installers without rebuilding one.
    Downloads land in a temp dir; install_sam() copies from there."""
    bundled = resource_path(PAYLOAD_NAME)
    if os.path.isfile(bundled):
        if on_progress:
            on_progress(hi, None)
        return bundled, resource_path(TS_PAYLOAD_NAME)

    tmp = tempfile.mkdtemp(prefix="sam_install_")
    exe_dst = os.path.join(tmp, EXE_ASSET)
    ts_dst = os.path.join(tmp, TS_ASSET)

    span = hi - lo
    _download(RELEASE_LATEST + "/" + EXE_ASSET, exe_dst, "the S.A.M barony.exe",
              on_progress, lo, lo + span * 0.75)
    _verify_exe(exe_dst)

    # typescript.js is optional — only .ts mod scripts need it. Never fail the whole
    # install because the TypeScript compiler didn't come down.
    try:
        _download(RELEASE_LATEST + "/" + TS_ASSET, ts_dst, "typescript.js",
                  on_progress, lo + span * 0.75, hi)
    except RuntimeError:
        ts_dst = None

    return exe_dst, ts_dst


def install_sam(barony_exe, sam_src, ts_src=None, on_progress=None):
    """Perform the actual install: back up barony.exe -> barony_vanilla.exe (only
    if no backup exists yet, so we never clobber a real vanilla with an already-
    patched exe), then copy sam_src over barony.exe. Reports via
    on_progress(fraction_0_to_1, status_text_or_None). Raises on any failure —
    the caller shows the error; the original file is left in place on failure.
    This is GUI-independent so it can be tested directly."""
    if not os.path.isfile(sam_src):
        raise FileNotFoundError(
            "The S.A.M barony.exe to install could not be found. If this installer was "
            "meant to download it, the download did not complete — try again."
        )

    def report(frac, status=None):
        if on_progress:
            on_progress(max(0.0, min(1.0, frac)), status)

    def copy_chunked(src, dst, lo, hi):
        total = os.path.getsize(src) or 1
        done = 0
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            while True:
                chunk = fi.read(1024 * 1024)
                if not chunk:
                    break
                fo.write(chunk)
                done += len(chunk)
                report(lo + (hi - lo) * (done / total))
        try:
            shutil.copystat(src, dst)
        except OSError:
            pass

    barony_dir = os.path.dirname(barony_exe)
    vanilla = os.path.join(barony_dir, "barony_vanilla.exe")

    if not os.path.exists(vanilla):
        report(0.0, "Backing up your original barony.exe → barony_vanilla.exe…")
        copy_chunked(barony_exe, vanilla, 0.0, 0.4)
    else:
        report(0.4, "Existing backup found — keeping barony_vanilla.exe.")

    report(0.4, "Installing the S.A.M Framework barony.exe…")
    copy_chunked(sam_src, barony_exe, 0.4, 0.8)

    # typescript.js — the TypeScript compiler for .ts mod scripts. Lua and
    # JavaScript scripts work without it, but .ts needs it deployed next to the
    # exe. Bundled in the installer; deployed into the same Barony folder.
    if ts_src and os.path.isfile(ts_src):
        report(0.8, "Installing typescript.js (TypeScript compiler)…")
        copy_chunked(ts_src, os.path.join(barony_dir, "typescript.js"), 0.8, 1.0)
    report(1.0, None)


# --------------------------------------------------------------------------- #
#  GUI
# --------------------------------------------------------------------------- #
import tkinter as tk
from tkinter import ttk, filedialog, messagebox


class Installer(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        self.configure(bg=STONE)
        self.resizable(False, False)
        self._center(600, 650)

        self.barony_path = find_barony()  # may be None

        self._build_header()
        # Persistent attribution footer — the always-visible "about" line
        # (BSD 2-Clause requires the Barony notice be reproduced in the binary).
        footer = tk.Frame(self, bg=STONE)
        footer.pack(side="bottom", fill="x")
        tk.Label(footer, text=ATTRIBUTION, bg=STONE, fg=GOLD_DIM,
                 font=(FONT_FAMILY, 8)).pack(pady=(5, 7))
        tk.Frame(self, bg=BORDER, height=1).pack(side="bottom", fill="x", padx=18)

        self.content = tk.Frame(self, bg=STONE)
        self.content.pack(fill="both", expand=True, padx=18, pady=(4, 8))

        self._init_progress_style()
        self.show_plan()

    # ----- window helpers --------------------------------------------------- #
    def _center(self, w, h):
        self.update_idletasks()
        x = (self.winfo_screenwidth() - w) // 2
        y = (self.winfo_screenheight() - h) // 3
        self.geometry(f"{w}x{h}+{x}+{y}")

    def _init_progress_style(self):
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(
            "SAM.Horizontal.TProgressbar",
            troughcolor=WELL, bordercolor=BORDER,
            background=GOLD, lightcolor=GOLD_HI, darkcolor=GOLD_DIM,
            thickness=18,
        )

    def _clear(self):
        for w in self.content.winfo_children():
            w.destroy()

    # ----- reusable widgets ------------------------------------------------- #
    def _panel(self, parent, title=None):
        outer = tk.Frame(parent, bg=BORDER)
        inner = tk.Frame(outer, bg=WOOD)
        inner.pack(fill="both", expand=True, padx=1, pady=1)
        if title:
            hdr = tk.Frame(inner, bg=WOOD)
            hdr.pack(fill="x", padx=12, pady=(10, 2))
            tk.Label(hdr, text=f"◆  {title}", bg=WOOD, fg=GOLD_HI,
                     font=(FONT_FAMILY, 12, "bold")).pack(anchor="w")
        body = tk.Frame(inner, bg=WOOD)
        body.pack(fill="both", expand=True, padx=12, pady=(2, 12))
        return outer, body

    def _gold_button(self, parent, text, command, primary=False, enabled=True,
                     full=False, big=False):
        # `full` -> button fills its parent's width; `big` -> larger padded label.
        edge = tk.Frame(parent, bg=GOLD if enabled and (primary or big) else GOLD_DIM)
        fg = (GOLD_HI if (primary or big) else GOLD) if enabled else "#5b4a26"
        btn = tk.Button(
            edge, text=text, command=command if enabled else None,
            bg=WOOD, fg=fg, activebackground=WELL, activeforeground=GOLD_HI,
            font=(FONT_FAMILY, 14 if big else 11, "bold" if (primary or big) else "normal"),
            relief="flat", bd=0, padx=20, pady=13 if big else 9,
            cursor="hand2" if enabled else "arrow",
            state="normal" if enabled else "disabled",
            disabledforeground="#5b4a26",
        )
        btn.pack(fill="x" if full else None, padx=2, pady=2)
        return edge

    # ----- header ----------------------------------------------------------- #
    def _build_header(self):
        head = tk.Frame(self, bg=STONE)
        head.pack(fill="x", pady=(16, 0))
        tk.Label(head, text="◆   S.A.M  FRAMEWORK   ◆", bg=STONE, fg=GOLD,
                 font=(FONT_FAMILY, 24, "bold")).pack()
        tk.Label(head, text="—  Support All Mods  —", bg=STONE, fg=GOLD_DIM,
                 font=(FONT_FAMILY, 10)).pack(pady=(2, 0))
        tk.Label(head, text=f"Installer  v{APP_VERSION}", bg=STONE, fg=PARCH,
                 font=(FONT_FAMILY, 9)).pack()
        tk.Frame(self, bg=BORDER, height=1).pack(fill="x", padx=18, pady=(12, 0))

    # ----- screen: plan (detect + what-will-happen + install) --------------- #
    def show_plan(self):
        self._clear()

        # --- Barony location panel ---
        loc_outer, loc = self._panel(self.content, "Barony Location")
        loc_outer.pack(fill="x", pady=(4, 10))

        if self.barony_path:
            row = tk.Frame(loc, bg=WOOD)
            row.pack(fill="x", anchor="w")
            tk.Label(row, text="✔", bg=WOOD, fg=GREEN,
                     font=(FONT_FAMILY, 14, "bold")).pack(side="left")
            tk.Label(row, text="  Barony found", bg=WOOD, fg=GREEN,
                     font=(FONT_FAMILY, 12, "bold")).pack(side="left")
            tk.Label(loc, text=os.path.dirname(self.barony_path), bg=WOOD, fg=PARCH,
                     font=("Consolas", 9), wraplength=520, justify="left").pack(anchor="w", pady=(4, 0))
        else:
            row = tk.Frame(loc, bg=WOOD)
            row.pack(fill="x", anchor="w")
            tk.Label(row, text="✖", bg=WOOD, fg=RED,
                     font=(FONT_FAMILY, 14, "bold")).pack(side="left")
            tk.Label(row, text="  Barony not found automatically", bg=WOOD, fg=RED,
                     font=(FONT_FAMILY, 12, "bold")).pack(side="left")
            tk.Label(loc, text="Click Browse and select your Barony folder "
                               "(the one containing barony.exe).",
                     bg=WOOD, fg=PARCH, font=(FONT_FAMILY, 10),
                     wraplength=520, justify="left").pack(anchor="w", pady=(4, 0))

        self._gold_button(loc, "Browse…", self.browse).pack(anchor="w", pady=(10, 0))

        # --- what will happen panel ---
        plan_outer, plan = self._panel(self.content, "What This Installer Will Do")
        plan_outer.pack(fill="x", pady=(0, 10))
        steps = [
            "Back up your original  barony.exe  →  barony_vanilla.exe",
            "Install the S.A.M Framework  barony.exe",
            "Install  typescript.js  (TypeScript compiler for mod scripting)",
            "On first launch, a  sam_log.txt  appears to confirm S.A.M is running",
        ]
        for s in steps:
            r = tk.Frame(plan, bg=WOOD)
            r.pack(fill="x", anchor="w", pady=1)
            tk.Label(r, text="✔", bg=WOOD, fg=GOLD, font=(FONT_FAMILY, 11)).pack(side="left")
            tk.Label(r, text="  " + s, bg=WOOD, fg=PARCH, font=(FONT_FAMILY, 10),
                     wraplength=500, justify="left").pack(side="left")

        # warn if a previous S.A.M install exists
        if self.barony_path:
            vanilla = os.path.join(os.path.dirname(self.barony_path), "barony_vanilla.exe")
            if os.path.exists(vanilla):
                tk.Label(plan,
                         text="⚠  A previous S.A.M install was detected. Your original "
                              "backup will be KEPT (not overwritten); S.A.M will be re-installed.",
                         bg=WOOD, fg=AMBER, font=(FONT_FAMILY, 9),
                         wraplength=510, justify="left").pack(anchor="w", pady=(8, 0))

        tk.Label(self.content,
                 text="Nothing is deleted. To return to vanilla anytime, rename "
                      "barony_vanilla.exe back to barony.exe.",
                 bg=STONE, fg=GOLD_DIM, font=(FONT_FAMILY, 9),
                 wraplength=540, justify="left").pack(anchor="w", pady=(0, 12))

        # --- prominent, full-width Install button (top-packed so it can't clip) ---
        self._gold_button(
            self.content, "Install S.A.M Framework", self.start_install,
            enabled=bool(self.barony_path), full=True, big=True,
        ).pack(fill="x", pady=(0, 4))
        if not self.barony_path:
            tk.Label(self.content,
                     text="Select your Barony folder above to enable install.",
                     bg=STONE, fg=RED, font=(FONT_FAMILY, 9)).pack(pady=(0, 2))

        self._gold_button(self.content, "Close", self.destroy).pack(pady=(2, 0))

    # ----- browse ----------------------------------------------------------- #
    def browse(self):
        folder = filedialog.askdirectory(title="Select your Barony folder (contains barony.exe)")
        if not folder:
            return
        candidate = os.path.join(folder, "barony.exe")
        if os.path.isfile(candidate):
            self.barony_path = candidate
            self.show_plan()
        else:
            messagebox.showerror(
                APP_TITLE,
                "No barony.exe was found in that folder.\n\n"
                "Pick the folder that contains barony.exe — usually:\n"
                r"...\Steam\steamapps\common\Barony",
            )

    # ----- screen: installing ---------------------------------------------- #
    def show_installing(self):
        self._clear()
        outer, body = self._panel(self.content, "Installing")
        outer.pack(fill="both", expand=True, pady=(30, 30))
        self.status_lbl = tk.Label(body, text="Preparing…", bg=WOOD, fg=PARCH,
                                    font=(FONT_FAMILY, 11), wraplength=500, justify="left")
        self.status_lbl.pack(anchor="w", pady=(20, 12))
        self.pb = ttk.Progressbar(body, style="SAM.Horizontal.TProgressbar",
                                  mode="determinate", maximum=100, length=520)
        self.pb.pack(pady=(0, 24))

    def start_install(self):
        if not self.barony_path:
            return
        self.show_installing()
        # Thread-safe hand-off: the worker touches ONLY this queue, never Tk.
        # A poller on the MAIN thread (scheduled via after) drains it and does
        # every UI update — the one correct way to update tkinter from a thread.
        self._queue = queue.Queue()
        threading.Thread(target=self._install_worker, daemon=True).start()
        self.after(50, self._pump_queue)

    def _install_worker(self):
        # Runs OFF the main thread. Must not call any tkinter method — only the
        # queue, which is thread-safe.
        try:
            # Fetch first (bundled build: instant; lean build: downloads the newest
            # release), then run the same proven copy/backup step over it.
            sam_src, ts_src = resolve_payload(
                on_progress=lambda frac, status: self._queue.put(("progress", frac, status)),
                lo=0.0, hi=0.6,
            )
            install_sam(
                self.barony_path, sam_src, ts_src,
                on_progress=lambda frac, status: self._queue.put(("progress", 0.6 + frac * 0.4, status)),
            )
            self._queue.put(("done", None, None))
        except Exception as exc:  # noqa: BLE001 - surface everything to the user
            self._queue.put(("error", exc, None))

    def _pump_queue(self):
        # Runs on the MAIN thread — the only place that touches Tk widgets.
        try:
            while True:
                kind, a, b = self._queue.get_nowait()
                if kind == "progress":
                    if b is not None:
                        self.status_lbl.configure(text=b)
                    self.pb.configure(value=max(0, min(100, a * 100)))
                elif kind == "done":
                    self.show_success()
                    return
                elif kind == "error":
                    self.show_failure(a)
                    return
        except queue.Empty:
            pass
        self.after(50, self._pump_queue)

    # ----- screen: success -------------------------------------------------- #
    def show_success(self):
        self._clear()
        tk.Label(self.content, text="✔", bg=STONE, fg=GREEN,
                 font=(FONT_FAMILY, 40)).pack(pady=(8, 0))
        tk.Label(self.content, text=f"S.A.M Framework v{APP_VERSION} installed successfully!",
                 bg=STONE, fg=GOLD_HI, font=(FONT_FAMILY, 15, "bold"),
                 wraplength=540).pack(pady=(2, 10))

        outer, body = self._panel(self.content, "Next Steps")
        outer.pack(fill="x", pady=(0, 10))
        for i, step in enumerate([
            "Launch Barony from Steam.",
            "Open the  Mods  menu.",
            "Enable the mods you want to play.",
            "Check  sam_log.txt  in your Barony folder to confirm S.A.M is running.",
        ], start=1):
            r = tk.Frame(body, bg=WOOD)
            r.pack(fill="x", anchor="w", pady=1)
            tk.Label(r, text=f"{i}.", bg=WOOD, fg=GOLD, font=(FONT_FAMILY, 11, "bold"),
                     width=2).pack(side="left")
            tk.Label(r, text=" " + step, bg=WOOD, fg=PARCH, font=(FONT_FAMILY, 10),
                     wraplength=490, justify="left").pack(side="left")

        tk.Label(self.content,
                 text="Build with the free Mod Builder:  "
                      "https://spider12223.github.io/SAM-Framework/",
                 bg=STONE, fg=GOLD_DIM, font=(FONT_FAMILY, 9),
                 wraplength=540, justify="left").pack(anchor="w", pady=(0, 4))
        tk.Label(self.content, text=ATTRIBUTION, bg=STONE, fg=GOLD_DIM,
                 font=(FONT_FAMILY, 9)).pack(anchor="w", pady=(0, 8))

        actions = tk.Frame(self.content, bg=STONE)
        actions.pack(fill="x", side="bottom")
        self._gold_button(actions, "Open Barony Folder", self.open_folder).pack(side="left")
        self._gold_button(actions, "Close", self.destroy, primary=True).pack(side="right")

    def open_folder(self):
        try:
            folder = os.path.dirname(self.barony_path)
            os.startfile(folder)  # noqa: S606 - intended: open in Explorer
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(APP_TITLE, f"Could not open the folder:\n{exc}")

    # ----- screen: failure -------------------------------------------------- #
    def show_failure(self, exc):
        self._clear()
        tk.Label(self.content, text="✖", bg=STONE, fg=RED,
                 font=(FONT_FAMILY, 40)).pack(pady=(8, 0))
        tk.Label(self.content, text="Installation could not finish",
                 bg=STONE, fg=RED, font=(FONT_FAMILY, 15, "bold")).pack(pady=(2, 10))

        outer, body = self._panel(self.content, "What happened")
        outer.pack(fill="both", expand=True, pady=(0, 10))
        tk.Label(body, text=self._friendly_error(exc), bg=WOOD, fg=PARCH,
                 font=(FONT_FAMILY, 10), wraplength=510, justify="left").pack(anchor="w", pady=(4, 6))
        tk.Label(body, text=f"Details: {type(exc).__name__}: {exc}", bg=WOOD, fg=GOLD_DIM,
                 font=("Consolas", 8), wraplength=510, justify="left").pack(anchor="w")

        actions = tk.Frame(self.content, bg=STONE)
        actions.pack(fill="x", side="bottom")
        self._gold_button(actions, "Back", self.show_plan).pack(side="left")
        self._gold_button(actions, "Close", self.destroy, primary=True).pack(side="right")

    @staticmethod
    def _friendly_error(exc):
        if isinstance(exc, PermissionError):
            return ("Windows blocked writing to the Barony folder.\n\n"
                    "• Make sure Barony is completely closed, then try again.\n"
                    "• If it still fails, right-click this installer and choose "
                    "“Run as administrator” (Steam game folders can need it).")
        if isinstance(exc, FileNotFoundError):
            return str(exc)
        if isinstance(exc, OSError) and getattr(exc, "winerror", None) == 32:
            return ("barony.exe is in use, so it can't be replaced.\n\n"
                    "Close Barony (and the Steam overlay) completely, then try again.")
        return ("Something went wrong while copying the files. Your original "
                "barony.exe was not removed. See the details below and try again.")


# --------------------------------------------------------------------------- #
#  Entry point
# --------------------------------------------------------------------------- #
def main():
    if "--selftest" in sys.argv:
        steam = find_steam()
        barony = find_barony()
        print("steam_install_path :", steam)
        print("steam_libraries    :", steam_library_roots(steam))
        print("barony_exe         :", barony)
        bundled = os.path.isfile(resource_path(PAYLOAD_NAME))
        print("payload_bundled    :", bundled, "(", resource_path(PAYLOAD_NAME), ")")
        print("payload_source     :", "bundled (offline)" if bundled
              else "download " + RELEASE_LATEST + "/" + EXE_ASSET)
        print("RESULT             :", "DETECTED" if barony else "NOT DETECTED")
        return 0

    if "--selftest-download" in sys.argv:
        # Exercise the real download + verification path end-to-end WITHOUT touching any
        # game files, so the fetch can be tested before shipping an installer.
        print("source             :", RELEASE_LATEST + "/" + EXE_ASSET)
        try:
            tmp = tempfile.mkdtemp(prefix="sam_selftest_")
            exe = os.path.join(tmp, EXE_ASSET)
            _download(RELEASE_LATEST + "/" + EXE_ASSET, exe, "barony.exe",
                      on_progress=lambda f, s: None)
            _verify_exe(exe)
            with open(exe, "rb") as f:
                head = f.read(2)
            print("downloaded_bytes   :", os.path.getsize(exe))
            print("pe_header          :", head.decode("latin-1"))
            print("verify_exe         : PASSED")
            shutil.rmtree(tmp, ignore_errors=True)
            print("RESULT             : DOWNLOAD OK")
            return 0
        except Exception as exc:  # noqa: BLE001 - selftest surfaces everything
            print("RESULT             : DOWNLOAD FAILED")
            print("error              :", exc)
            return 1
    Installer().mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
