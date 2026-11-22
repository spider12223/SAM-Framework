"""Layout check for the installer's plan screen, on a short display and on a tall one.

    python installer/layout_test.py [path/to/installer.py]

A player on a short display could not read "Please Read First": the window stops growing at the
display's height and the panels at the bottom were cut off with no way to scroll to them. This
builds the real window from installer.py (nothing is downloaded or installed) with the display
height faked, and checks with Tk's own geometry, not by eye:

  short (600 px): the scrollbar is up, Install and Close are on screen, the end of the
                  "Please Read First" text is out of view at first and in view after scrolling
                  to the bottom, and one turn of the mouse wheel over a panel's text scrolls the
                  area; another screen drops the wheel, and the plan screen built again (Back,
                  Browse) has one scrolling area that the wheel still scrolls;
  tall (1400 px): no scrollbar, and the whole screen shows without scrolling.

Exit code 0 when every check passes, 1 otherwise.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Another installer.py may be named, to see these checks fail on the layout before the fix.
SOURCE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "installer.py")
spec = importlib.util.spec_from_file_location("sam_installer", SOURCE)
inst = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inst)

passed = failed = 0


def check(name, ok, saw=""):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
    print(("PASS  " if ok else "FAIL  ") + name + (f"   ({saw})" if saw else ""))


def settle(app):
    # A geometry change takes several update cycles before the measurements are true.
    for _ in range(8):
        app.update()


def walk(widget):
    yield widget
    for child in widget.winfo_children():
        yield from walk(child)


def find_text(app, needle):
    for w in walk(app):
        try:
            if needle in str(w.cget("text")):
                return w
        except Exception:  # noqa: BLE001 - frames and canvases have no text option
            pass
    return None


def y_in(widget, ancestor):
    """The widget's top edge in `ancestor`'s coordinates, from the placement of each parent."""
    y = 0
    w = widget
    while w is not None and w != ancestor:
        y += w.winfo_y()
        w = w.master
    return y


def end_of_warning_visible(app, warn):
    """Is the last line of the warning text on screen, inside the area the panels are drawn in?"""
    if warn is None or not warn.winfo_ismapped() or warn.winfo_height() < warn.winfo_reqheight():
        return False
    canvas = getattr(app, "_scroll_canvas", None)
    if canvas is not None:
        return y_in(warn, app._scroll_inner) + warn.winfo_height() <= canvas.canvasy(canvas.winfo_height()) + 1
    # The layout before the fix: the panels end where the action buttons begin.
    install = find_text(app, "Install S.A.M Framework")
    limit = install.master.master.winfo_rooty() if install is not None else app.winfo_rooty() + app.winfo_height()
    return warn.winfo_rooty() + warn.winfo_height() <= limit


def build(screen_height):
    inst.find_barony = lambda: r"C:\NoSuchFolder\Barony\barony.exe"   # a path, so Install is enabled
    inst.Installer.winfo_screenheight = lambda self: screen_height
    app = inst.Installer()
    settle(app)
    return app


# ---- a short display ------------------------------------------------------------------------
app = build(600)
canvas, inner = getattr(app, "_scroll_canvas", None), getattr(app, "_scroll_inner", None)
bars = [w for w in walk(app) if w.winfo_class() == "Scrollbar"]
check("SHORT: the plan screen has a scrolling area", canvas is not None and inner is not None)
check("SHORT: the window stops at the display's height", app.winfo_height() <= 600 - 80, f"{app.winfo_height()} px")
check("SHORT: the scrollbar is shown", bool(bars) and bars[0].winfo_ismapped())
install = find_text(app, "Install S.A.M Framework")
close = find_text(app, "Close")
check("SHORT: Install is on screen", install is not None and install.winfo_ismapped()
      and install.winfo_rooty() + install.winfo_height() <= app.winfo_rooty() + app.winfo_height())
check("SHORT: Close is on screen", close is not None and close.winfo_ismapped()
      and close.winfo_rooty() + close.winfo_height() <= app.winfo_rooty() + app.winfo_height())
warn = find_text(app, "you lose three things")
check("SHORT: the end of Please Read First starts out of view (the case being fixed)",
      warn is not None and not end_of_warning_visible(app, warn))
def wheel_over_panel_text(app):
    """Turn the wheel once over a panel's title text, deep inside the scrolling area, the way a pointer
    resting on the panels sends it. True when the area scrolled down."""
    canvas = getattr(app, "_scroll_canvas", None)
    title = find_text(app, "Barony Location")
    if canvas is None or title is None:
        return False, "nothing scrolls"
    canvas.yview_moveto(0.0)
    settle(app)
    top_before = canvas.canvasy(0)
    title.event_generate("<MouseWheel>", delta=-120, x=5, y=5)
    settle(app)
    return canvas.canvasy(0) > top_before, f"view top {top_before:.0f} -> {canvas.canvasy(0):.0f}"


ok, saw = wheel_over_panel_text(app)
check("SHORT: one turn of the mouse wheel over a panel's text scrolls down", ok, saw)
if canvas is not None:
    canvas.yview_moveto(1.0)
    settle(app)
check("SHORT: scrolled to the bottom, all of Please Read First is in view", end_of_warning_visible(app, warn))
# Another screen has nothing to scroll, so it must not keep the wheel; Back (show_plan) brings it back,
# and so does Browse, which builds the plan screen again over itself.
app.show_failure(RuntimeError("layout test"))
settle(app)
check("SCREENS: the failure screen holds no mouse-wheel binding", app.bind_all("<MouseWheel>") == "",
      repr(app.bind_all("<MouseWheel>"))[:40])
app.show_plan()
app.show_plan()
settle(app)
canvases = [w for w in walk(app) if w.winfo_class() == "Canvas"]
check("SCREENS: the plan screen built twice has one scrolling area", len(canvases) == 1, f"{len(canvases)} canvas(es)")
ok, saw = wheel_over_panel_text(app)
check("SCREENS: after Back, the wheel over a panel's text still scrolls", ok, saw)
app.destroy()

# ---- a tall display -------------------------------------------------------------------------
app = build(1400)
canvas = getattr(app, "_scroll_canvas", None)
bars = [w for w in walk(app) if w.winfo_class() == "Scrollbar"]
check("TALL: no scrollbar", not bars or not bars[0].winfo_ismapped())
warn = find_text(app, "you lose three things")
check("TALL: Please Read First shows whole without scrolling", end_of_warning_visible(app, warn)
      and (canvas is None or canvas.canvasy(0) == 0))
install = find_text(app, "Install S.A.M Framework")
check("TALL: Install is on screen", install is not None and install.winfo_ismapped())
app.destroy()

print(f"LAYOUT CHECK: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
