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
# No version number on purpose. This installer always pulls the NEWEST barony.exe from
# the GitHub "latest" release, so it never goes out of date and never needs rebuilding.
# The 2026-08 revision reintroduced a hardcoded installer version and, worse, printed it
# as the FRAMEWORK version on the success screen: an installer built at 1.3.0 announced
# "v1.3.0 installed" while actually fetching and installing 2.5.0. check_versions.py
# records the version-less design as a deliberately retired check. Do not add it back.
APP_TITLE = "S.A.M Framework Installer"
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


BARONY_APPID = "371970"


def find_barony():
    """Full path to <library>\\steamapps\\common\\Barony\\barony.exe, or None.

    Prefers the library Steam actually OWNS the app in, identified by its appmanifest. The
    old version returned the first root with a Barony folder, so a leftover folder in another
    library -- or a copy on a drive Steam no longer launches from -- won, and the installer
    patched a game the user was not playing.
    """
    roots = steam_library_roots(find_steam())
    fallback = None
    for root in roots:
        candidate = os.path.join(root, "steamapps", "common", "Barony", "barony.exe")
        if not os.path.isfile(candidate):
            continue
        manifest = os.path.join(root, "steamapps", "appmanifest_%s.acf" % BARONY_APPID)
        if os.path.isfile(manifest):
            return candidate
        if fallback is None:
            fallback = candidate
    return fallback


def _download(url, dst, label, on_progress=None, lo=0.0, hi=1.0):
    """Stream `url` to `dst`, reporting progress into the [lo, hi] slice.
    Raises a player-readable RuntimeError on any network failure."""
    req = urllib.request.Request(url, headers={"User-Agent": "SAM-Framework-Installer"})
    # Which side of the transfer we are on. resp.read() and out.write() share one try block,
    # and the handler below used to guess by isinstance(URLError, TimeoutError) -- but
    # ConnectionResetError is neither, so a dropped connection was reported as "Couldn't save
    # to disk". Recording the phase makes the distinction exact instead of inferred.
    phase = ["read"]
    try:
        with urllib.request.urlopen(req, timeout=30) as resp, open(dst, "wb") as out:
            total = 0
            try:
                total = int(resp.headers.get("Content-Length") or 0)
            except (TypeError, ValueError):
                total = 0
            done = 0
            while True:
                phase[0] = "read"
                chunk = resp.read(256 * 1024)
                if not chunk:
                    break
                phase[0] = "write"
                out.write(chunk)
                done += len(chunk)
                if on_progress:
                    frac = (done / total) if total else 0.0
                    mb = done / (1024 * 1024)
                    text = "Downloading %s… %.1f MB" % (label, mb)
                    if total:
                        text += " of %.1f MB" % (total / (1024 * 1024))
                    on_progress(lo + (hi - lo) * min(1.0, frac), text)
            # A short read is NOT an error to urllib: read() simply returns b"" and the loop
            # ends. Without this comparison a connection cut at 60% produced a 7 MB file that
            # sailed through the 4 MB size floor and was installed as the user's barony.exe.
            if total and done != total:
                raise RuntimeError(
                    "The download of %s was cut off: %.1f MB arrived out of %.1f MB.\n\n"
                    "This is almost always a dropped connection. Nothing in your Barony "
                    "folder was changed. Please try again."
                    % (label, done / (1024.0 * 1024.0), total / (1024.0 * 1024.0))
                )
    except urllib.error.HTTPError as exc:
        raise RuntimeError(
            "Couldn't download %s from GitHub (HTTP %s).\n\n"
            "The release may still be publishing. Wait a minute and try again, or grab "
            "%s manually from:\n%s" % (label, exc.code, label, RELEASE_LATEST)
        ) from exc
    except OSError as exc:
        # A write failure is NOT a network failure. out.write() lives inside this try, so a
        # full disk (ENOSPC) used to surface as "check your internet connection" -- and since
        # every failed attempt also leaks its temp folder, retrying filled the disk further
        # while still blaming the network. Split them apart.
        import errno as _errno
        if getattr(exc, "errno", None) == _errno.ENOSPC:
            raise RuntimeError(
                "Your drive is full, so %s could not be saved.\n\n"
                "Free up about 100 MB and try again. Nothing in your Barony folder was "
                "changed." % label
            ) from exc
        if phase[0] == "write":
            raise RuntimeError(
                "Couldn't save %s to disk.\n\nDetails: %s" % (label, exc)
            ) from exc
        raise RuntimeError(
            "Couldn't reach GitHub to download %s.\n\n"
            "This installer fetches the latest S.A.M build at install time, so it needs "
            "an internet connection.\n\n"
            "Note it downloads from TWO addresses: github.com and "
            "release-assets.githubusercontent.com. Some school, office and DNS-filtered "
            "networks allow the first and block the second. If your connection is fine, "
            "that is the likely cause -- try another network, or download barony.exe "
            "manually from the releases page and copy it over yourself.\n\nDetails: %s" % (label, exc)
        ) from exc


SAM_EXE_MARKER = b"S.A.M Framework"   # present in our build, absent from vanilla

# Files and folders that a real Barony install has and a bare payload folder does not. The
# Workshop item and mods/sam_framework BOTH contain a barony.exe, so "has a barony.exe" was
# never enough to identify the game folder: picking one of them reached the "already S.A.M,
# no backup" branch, which tells the user to run Steam's Verify integrity -- and that wipes
# the S.A.M install they already had. Requiring several markers keeps this working if a
# future Barony release moves one of them.
BARONY_MARKERS = ("data", "books", "editor.exe", "SDL2.dll", "fmod.dll")
MIN_BARONY_MARKERS = 3


def looks_like_barony_folder(folder):
    """True if `folder` is a real Barony install and not just something holding a barony.exe."""
    try:
        hits = sum(1 for m in BARONY_MARKERS if os.path.exists(os.path.join(folder, m)))
    except OSError:
        return False
    return hits >= MIN_BARONY_MARKERS


def _pe_is_complete(path):
    """True only if every section the PE header declares is actually present in the file.

    A size floor cannot see truncation. MIN_EXE_BYTES is 4 MB while a real barony.exe is
    about 13 MB, so a copy interrupted anywhere in that 9 MB window passed the old check,
    counted as a valid vanilla backup, and let the genuine original be overwritten. The PE
    header states where each section's bytes end, so comparing that against the file size
    detects truncation exactly, at any size, with no guessing.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(0x40)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return False
            e_lfanew = int.from_bytes(head[0x3C:0x40], "little")
            f.seek(e_lfanew)
            sig = f.read(4)
            if sig[:2] != b"PE" or sig[2:] != bytes(2):
                return False
            coff = f.read(20)
            if len(coff) < 20:
                return False
            n_sections = int.from_bytes(coff[2:4], "little")
            opt_size = int.from_bytes(coff[16:18], "little")
            if not 0 < n_sections <= 96:
                return False
            f.seek(e_lfanew + 24 + opt_size)
            needed = 0
            for _ in range(n_sections):
                sec = f.read(40)
                if len(sec) < 40:
                    return False
                raw_size = int.from_bytes(sec[16:20], "little")
                raw_ptr = int.from_bytes(sec[20:24], "little")
                if raw_ptr:                     # 0 means the section has no bytes on disk
                    needed = max(needed, raw_ptr + raw_size)
            return needed > 0 and os.path.getsize(path) >= needed
    except (OSError, ValueError):
        return False


def _looks_like_real_exe(path):
    """A plausible, COMPLETE Windows executable -- not a truncated leftover."""
    try:
        if not os.path.isfile(path) or os.path.getsize(path) < MIN_EXE_BYTES:
            return False
    except OSError:
        return False
    return _pe_is_complete(path)


def _is_sam_exe(path, on_error):
    """True if this file is a S.A.M build rather than the stock game.

    `on_error` is REQUIRED, because the safe answer differs by caller and the old version
    silently returned False for both. False means "this is not S.A.M": at the backup check
    that is the cautious answer, but at the "the live exe is already S.A.M, refuse" guard it
    is the dangerous one. An exe momentarily locked by Steam read as "not S.A.M, safe to back
    up", and S.A.M was saved as the user's vanilla. Every caller now states what an unreadable
    file counts as.
    """
    try:
        with open(path, "rb") as f:
            return SAM_EXE_MARKER in f.read()
    except OSError:
        return on_error


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


MIN_TS_BYTES = 1024 * 1024        # the real typescript.js is ~9 MB


def _verify_typescript(path):
    """Reject a truncated typescript.js or an HTML error page saved under that name.

    It had no verification of any kind. install_sam's only gate was os.path.isfile, which a
    0-byte or half-written file satisfies, so garbage was copied over a working compiler and
    every .ts mod broke with nothing to explain it.
    """
    size = os.path.getsize(path) if os.path.isfile(path) else 0
    if size < MIN_TS_BYTES:
        raise RuntimeError(
            "The downloaded typescript.js is only %d bytes, which is far too small to be "
            "real. The download was probably interrupted." % size
        )
    with open(path, "rb") as f:
        head = f.read(256).lstrip()
    if head[:1] == b"<":
        raise RuntimeError(
            "The downloaded typescript.js is a web page, not the TypeScript compiler. "
            "GitHub most likely returned an error page."
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
        return bundled, resource_path(TS_PAYLOAD_NAME), None, None

    tmp = tempfile.mkdtemp(prefix="sam_install_")
    exe_dst = os.path.join(tmp, EXE_ASSET)
    ts_dst = os.path.join(tmp, TS_ASSET)

    span = hi - lo
    _download(RELEASE_LATEST + "/" + EXE_ASSET, exe_dst, "the S.A.M barony.exe",
              on_progress, lo, lo + span * 0.75)
    _verify_exe(exe_dst)

    # typescript.js is optional -- only .ts mod scripts need it -- so a failure here must not
    # fail the whole install. But it must not be SILENT either: the reason is carried back so
    # the success screen can say the compiler is missing instead of reporting a clean install
    # and leaving every .ts mod broken with no explanation.
    ts_error = None
    try:
        _download(RELEASE_LATEST + "/" + TS_ASSET, ts_dst, "typescript.js",
                  on_progress, lo + span * 0.75, hi)
        _verify_typescript(ts_dst)
    except RuntimeError as exc:
        ts_dst, ts_error = None, str(exc)

    # The temp dir is handed back so the caller can delete it. Nothing ever deleted it, so
    # every run left ~22 MB behind -- and the disk-full message told the user to free space
    # that the next attempt promptly consumed again.
    return exe_dst, ts_dst, ts_error, tmp


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
        # Write to a sibling temp file and os.replace() it into place. The old code did
        # open(dst, "wb"), which TRUNCATES the live barony.exe before the first byte is
        # written -- so any mid-write failure (antivirus, a lock, a full disk) left a
        # corrupt, unlaunchable game while the error screen claimed nothing had changed.
        # os.replace is atomic on the same volume, so barony.exe is either the old file or
        # the new one, never a half-written mixture.
        total = os.path.getsize(src) or 1
        done = 0
        tmp = dst + ".sam-tmp"
        try:
            with open(src, "rb") as fi, open(tmp, "wb") as fo:
                while True:
                    chunk = fi.read(1024 * 1024)
                    if not chunk:
                        break
                    fo.write(chunk)
                    done += len(chunk)
                    report(lo + (hi - lo) * (done / total))
                fo.flush()
                os.fsync(fo.fileno())
            try:
                shutil.copystat(src, tmp)
            except OSError:
                pass
            os.replace(tmp, dst)
        except BaseException:
            try:
                if os.path.exists(tmp):
                    os.remove(tmp)
            except OSError:
                pass
            raise

    barony_dir = os.path.dirname(barony_exe)
    vanilla = os.path.join(barony_dir, "barony_vanilla.exe")

    # The backup is the ONLY way back to vanilla, and the plan screen promises it works.
    # The old check was `if not os.path.exists(vanilla)` -- existence only. A truncated
    # backup left by a failed attempt therefore counted as "already backed up", the real
    # backup step was skipped, and the genuine vanilla exe was overwritten by the S.A.M
    # build. That destroyed the escape hatch permanently while the UI said otherwise.
    # Every branch below depends on judging barony.exe, and an unreadable one cannot be
    # judged. Say so plainly rather than guessing -- guessing here is exactly what let a
    # S.A.M build be saved as the user's vanilla backup.
    if not os.path.isfile(barony_exe):
        raise RuntimeError("barony.exe is missing from the folder you selected."
                           "\n\nNothing was changed.")
    try:
        with open(barony_exe, "rb") as _probe:
            _probe.read(1)
    except OSError as exc:
        raise RuntimeError(
            "barony.exe could not be read, so the installer cannot tell whether it is the "
            "original game or an existing S.A.M build."
            "\n\nClose Barony and the Steam overlay completely, then try again."
            "\n\nNothing was changed.\n\nDetails: %s" % exc
        ) from exc

    # An unreadable backup counts as NOT good, so we fall through and replace it.
    backup_is_good = _looks_like_real_exe(vanilla) and not _is_sam_exe(vanilla, on_error=True)
    if backup_is_good:
        report(0.4, "Existing backup found — keeping barony_vanilla.exe.")
    elif _is_sam_exe(barony_exe, on_error=True):
        # Current game exe is already S.A.M and we have no usable backup. Backing up now
        # would save S.A.M *as* vanilla and lose the original for good. Refuse instead.
        raise RuntimeError(
            "Your barony.exe is already a S.A.M build, and barony_vanilla.exe is missing "
            "or damaged, so there is no original left to back up.\n\n"
            "Nothing was changed. To restore the original game:\n"
            "1. Open Steam, right-click Barony, then Properties.\n"
            "2. Installed Files, then Verify integrity of game files.\n"
            "3. Run this installer again once that finishes."
        )
    else:
        # Validate the SOURCE before saving it as "your original". Only the destination was
        # ever checked, so a barony.exe that was itself truncated (an interrupted Steam
        # update, a bad disk) got copied over as the vanilla backup and the install reported
        # success, leaving the user with two broken exes and a promise that one was their game.
        if not _looks_like_real_exe(barony_exe):
            raise RuntimeError(
                "Your barony.exe is damaged or incomplete, so it cannot be backed up as your "
                "original game."
                "\n\nNothing was changed. To repair it:"
                "\n1. Open Steam, right-click Barony, then Properties."
                "\n2. Installed Files, then Verify integrity of game files."
                "\n3. Run this installer again once that finishes."
            )
        if os.path.exists(vanilla):
            report(0.0, "Existing backup looks damaged — replacing it…")
        else:
            report(0.0, "Backing up your original barony.exe → barony_vanilla.exe…")
        copy_chunked(barony_exe, vanilla, 0.0, 0.4)

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
        # Vertically resizable on purpose. The window used to be pinned to 650px while the
        # plan screen needs 975px, so the bottom of every screen was simply not drawn and the
        # user could not drag it taller. _fit_height() below sizes to content instead.
        self.resizable(False, True)
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

        self._installing = False
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        self._init_progress_style()
        self.show_plan()

    def _on_close(self):
        """Refuse to close mid-install. See start_install for why this matters: the worker
        cannot be interrupted cleanly, and killing it leaves a large temp file behind in the
        user's game folder with the install half-applied."""
        if getattr(self, "_installing", False):
            messagebox.showwarning(
                APP_TITLE,
                "The install is still running.\n\n"
                "Closing now could leave your Barony folder half-updated. Please wait for it "
                "to finish -- it only takes a few seconds.",
            )
            return
        self.destroy()

    # ----- window helpers --------------------------------------------------- #
    def _fit_height(self):
        """Grow the window to whatever the current screen needs, up to what fits on the
        display. Called after each screen is built, because the screens differ in height and
        show_plan() is re-entered by browse() and by the failure screen's Back button."""
        self.update_idletasks()
        want = self.winfo_reqheight()
        cap = self.winfo_screenheight() - 80          # leave room for the taskbar
        self.geometry("600x%d" % max(420, min(want, cap)))

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
        tk.Label(head, text="Installer", bg=STONE, fg=PARCH,
                 font=(FONT_FAMILY, 9)).pack()
        tk.Frame(self, bg=BORDER, height=1).pack(fill="x", padx=18, pady=(12, 0))

    # ----- screen: plan (detect + what-will-happen + install) --------------- #
    def show_plan(self):
        self._clear()

        # The action buttons are packed FIRST and to the BOTTOM, before any panel is added.
        # pack() hands out space in call order, so packing them last meant that on a window
        # shorter than the content they were never mapped -- not clipped, not drawn at all,
        # leaving a plan screen with no way to start the install and no way to close it.
        # Reserving their space up front means they survive no matter how tall the rest gets.
        actions = tk.Frame(self.content, bg=STONE)
        actions.pack(side="bottom", fill="x")

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

        # --- what you give up panel -------------------------------------------
        # S.A.M ships a barony.exe we build ourselves, and we cannot build it with Epic
        # Online Services: EOS needs Turning Wheel's private Epic product/sandbox/deployment
        # credentials, which are not public. So crossplay with Epic Games Store players is
        # gone while S.A.M is installed. A user reported this on the Workshop as a mystery
        # breakage because nothing anywhere told them. Say it BEFORE they click Install.
        warn_outer, warn = self._panel(self.content, "Please Read First")
        warn_outer.pack(fill="x", pady=(0, 10))
        tk.Label(warn,
                 text=("While S.A.M is installed you lose three things: crossplay with Epic "
                       "Games Store players, online leaderboard scores, and video playback "
                       "(the intro and animated signs). Everything else, including Steam "
                       "multiplayer, works normally.\n\n"
                       "These are not bugs we can fix: they need private Epic and PlayFab "
                       "credentials that only the game's developer has.\n\n"
                       "Your original game is kept as barony_vanilla.exe. To go back to it, "
                       "delete barony.exe and then rename barony_vanilla.exe to barony.exe "
                       "(renaming alone will not work, because both files exist). Steam's "
                       "Verify integrity of game files does the same thing for you. Your "
                       "saves and mods are untouched either way."),
                 bg=WOOD, fg=PARCH, font=(FONT_FAMILY, 9),
                 wraplength=520, justify="left").pack(anchor="w")


        # warn if a previous S.A.M install exists
        if self.barony_path:
            vanilla = os.path.join(os.path.dirname(self.barony_path), "barony_vanilla.exe")
            # Judge the backup the SAME way install_sam does. This used to be os.path.exists,
            # so the screen promised "your backup will be KEPT" on exactly the runs where the
            # backup is damaged and gets replaced, or where the install is refused outright.
            if _looks_like_real_exe(vanilla) and not _is_sam_exe(vanilla, on_error=True):
                tk.Label(plan,
                         text="⚠  A previous S.A.M install was detected. Your original "
                              "backup will be KEPT (not overwritten); S.A.M will be re-installed.",
                         bg=WOOD, fg=AMBER, font=(FONT_FAMILY, 9),
                         wraplength=510, justify="left").pack(anchor="w", pady=(8, 0))

        tk.Label(self.content,
                 text="Nothing is deleted. To return to vanilla anytime, delete barony.exe "
                      "and rename barony_vanilla.exe back to barony.exe.",
                 bg=STONE, fg=GOLD_DIM, font=(FONT_FAMILY, 9),
                 wraplength=540, justify="left").pack(anchor="w", pady=(0, 12))

        # --- prominent, full-width Install button (top-packed so it can't clip) ---
        self._gold_button(
            actions, "Install S.A.M Framework", self.start_install,
            enabled=bool(self.barony_path), full=True, big=True,
        ).pack(fill="x", pady=(0, 4))
        if not self.barony_path:
            tk.Label(actions,
                     text="Select your Barony folder above to enable install.",
                     bg=STONE, fg=RED, font=(FONT_FAMILY, 9)).pack(pady=(0, 2))

        self._gold_button(actions, "Close", self.destroy).pack(pady=(2, 0))
        self._fit_height()

    # ----- browse ----------------------------------------------------------- #
    def browse(self):
        folder = filedialog.askdirectory(title="Select your Barony folder (contains barony.exe)")
        if not folder:
            return
        candidate = os.path.join(folder, "barony.exe")
        if os.path.isfile(candidate) and not looks_like_barony_folder(folder):
            messagebox.showerror(
                APP_TITLE,
                "That folder has a barony.exe in it, but it is not a Barony installation.\n\n"
                "It looks like a S.A.M package folder (the Steam Workshop copy, or "
                "mods\\sam_framework). Installing into it would do nothing to your game.\n\n"
                "Pick the folder that also contains data, books and editor.exe. It is usually:\n"
                "Steam\\steamapps\\common\\Barony",
            )
            return
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
        self._fit_height()

    def start_install(self):
        if not self.barony_path:
            return
        # The worker is a daemon thread: closing the window kills it at its next GIL
        # acquisition WITHOUT unwinding, so copy_chunked's cleanup never runs and a ~13 MB
        # barony.exe.sam-tmp is orphaned in the game folder. There was no close handler at
        # all, and the installing screen has no buttons, so the title-bar X was unimpeded.
        self._installing = True
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
        tmp_dir = None
        try:
            # Fetch first (bundled build: instant; lean build: downloads the newest
            # release), then run the same proven copy/backup step over it.
            sam_src, ts_src, ts_error, tmp_dir = resolve_payload(
                on_progress=lambda frac, status: self._queue.put(("progress", frac, status)),
                lo=0.0, hi=0.6,
            )
            install_sam(
                self.barony_path, sam_src, ts_src,
                on_progress=lambda frac, status: self._queue.put(("progress", 0.6 + frac * 0.4, status)),
            )
            # ts_error rides along so the success screen can be honest about what landed.
            self._queue.put(("done", ts_error, None))
        except Exception as exc:  # noqa: BLE001 - surface everything to the user
            self._queue.put(("error", exc, None))
        finally:
            # Always clean up the download folder, on success and on failure alike.
            if tmp_dir:
                shutil.rmtree(tmp_dir, ignore_errors=True)
            self._queue.put(("finished", None, None))

    def _pump_queue(self):
        # Runs on the MAIN thread — the only place that touches Tk widgets.
        try:
            while True:
                kind, a, b = self._queue.get_nowait()
                if kind == "progress":
                    if b is not None:
                        self.status_lbl.configure(text=b)
                    self.pb.configure(value=max(0, min(100, a * 100)))
                elif kind == "finished":
                    self._installing = False
                elif kind == "done":
                    self._installing = False
                    self.show_success(ts_error=a)
                    return
                elif kind == "error":
                    self._installing = False
                    self.show_failure(a)
                    return
        except queue.Empty:
            pass
        self.after(50, self._pump_queue)

    # ----- screen: success -------------------------------------------------- #
    def show_success(self, ts_error=None):
        self._clear()
        tk.Label(self.content, text="✔", bg=STONE, fg=GREEN,
                 font=(FONT_FAMILY, 40)).pack(pady=(8, 0))
        tk.Label(self.content, text="S.A.M Framework installed successfully!",
                 bg=STONE, fg=GOLD_HI, font=(FONT_FAMILY, 15, "bold"),
                 wraplength=540).pack(pady=(2, 10))

        # Report a missing TypeScript compiler instead of claiming a clean install. This was
        # swallowed entirely: .ts mods simply failed later with nothing to point at.
        if ts_error:
            warn_outer, warn = self._panel(self.content, "One Thing Did Not Install")
            warn_outer.pack(fill="x", pady=(0, 10))
            tk.Label(warn,
                     text=("typescript.js could not be downloaded, so mods written in "
                           "TypeScript (.ts) will not run. Lua and JavaScript mods are "
                           "unaffected and S.A.M itself is installed correctly.\n\n"
                           "To fix it, download typescript.js from the releases page and put "
                           "it in your Barony folder next to barony.exe.\n\n"
                           "Reason: " + ts_error),
                     bg=WOOD, fg=AMBER, font=(FONT_FAMILY, 9),
                     wraplength=520, justify="left").pack(anchor="w")

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
        self._fit_height()

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
        self._fit_height()

    @staticmethod
    def _friendly_error(exc):
        # RuntimeError is what _download and _verify_exe raise, and they have already
        # written a precise user-facing message (which asset, which HTTP code, what to
        # do next). Without this branch it fell through to the catch-all and the user
        # was told their barony.exe "was not removed" -- an answer to a question they
        # never asked, about a step that had not run. Two separate Workshop bug reports
        # were this one missing isinstance check.
        if isinstance(exc, RuntimeError):
            return str(exc)
        # The lock check MUST come first. On CPython winerror 32/33 map to PermissionError,
        # so testing PermissionError first made this arm unreachable for exactly the case it
        # was written for: a running Barony told the user to run as administrator, which does
        # not help and does not work.
        if isinstance(exc, OSError) and getattr(exc, "winerror", None) in (32, 33, 1224):
            # 32 = sharing violation, 33 = lock violation, 1224 = user-mapped section open.
            # Antivirus real-time scanning trips 33 and 1224 as often as a running game
            # trips 32; the old check only caught 32.
            return ("barony.exe is open in another program, so it can't be replaced.\n\n"
                    "\u2022 Close Barony completely (check the system tray).\n"
                    "\u2022 Close the Steam client too.\n"
                    "\u2022 If it still fails, add your Barony folder to your antivirus "
                    "exclusions \u2014 real-time scanning holds the file open.")
        if isinstance(exc, PermissionError):
            return ("Windows blocked writing to the Barony folder.\n\n"
                    "• Make sure Barony is completely closed, then try again.\n"
                    "• If it still fails, right-click this installer and choose "
                    "“Run as administrator” (Steam game folders can need it).")
        if isinstance(exc, FileNotFoundError):
            return str(exc)
        # Last resort. Deliberately says NOTHING about barony.exe: by the time an unknown
        # error reaches here the install may be half-applied, and the old text promised the
        # opposite ("your original barony.exe was not removed"). A user hit that line after a
        # failed DOWNLOAD, where no file had been touched at all. Give the safe recovery.
        return ("The install stopped partway, so your Barony folder may be half-updated.\n\n"
                "Run this installer again before launching the game. If it keeps failing, "
                "see the details below.")


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
