#!/usr/bin/env python3
"""
hdr-cal — HDR calibration & test-pattern overlay for cosmic-hdr.

Usage:
  hdr-cal.py <pattern>           — fullscreen static colour field
  hdr-cal.py --calibrate ...     — Windows-style interactive calibration

Patterns: black darkgray gray50 white red green blue sdr_hdr

Interactive flags (--calibrate mode):
  --sdr-nits   <n>    initial SDR white nits   (default 203)
  --peak-nits  <n>    display peak nits         (default 800)
  --gamut      <n>    gamut expansion %          (default 100)
  --bpc        <n>    bit depth                  (default 10)
  --gamut-mode <s>    bt2020 | dci-p3 | srgb    (default bt2020)
"""

import sys
import subprocess
import gi
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, GLib

BIN = '/usr/local/bin/cosmic-hdr'

# ── Colour definitions ─────────────────────────────────────────────────────────

STATIC_COLORS = {
    'black':    (0.000, 0.000, 0.000),
    'darkgray': (0.046, 0.046, 0.046),   # ~5% luminance
    'gray50':   (0.500, 0.500, 0.500),
    'white':    (1.000, 1.000, 1.000),
    'red':      (1.000, 0.000, 0.000),
    'green':    (0.000, 1.000, 0.000),
    'blue':     (0.000, 0.000, 1.000),
}

# ── Utilities ──────────────────────────────────────────────────────────────────

def add_key_click_close(win, app):
    key = Gtk.EventControllerKey()
    key.connect('key-pressed', lambda c, k, *a: app.quit() if k == Gdk.KEY_Escape else False)
    win.add_controller(key)
    click = Gtk.GestureClick()
    click.connect('pressed', lambda *a: None)   # eat click so it doesn't propagate weirdly
    win.add_controller(click)


# ── Static pattern window ──────────────────────────────────────────────────────

class StaticPatternWindow(Gtk.ApplicationWindow):
    def __init__(self, app, pattern):
        super().__init__(application=app)
        self.set_fullscreened(True)
        add_key_click_close(self, app)

        if pattern == 'sdr_hdr':
            # Left = SDR-like gray, Right = pure white (HDR peak reference)
            box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
            for rgb in [(0.20, 0.20, 0.20), (1.0, 1.0, 1.0)]:
                da = Gtk.DrawingArea()
                r, g, b = rgb
                da.set_draw_func(lambda w, cr, _w, _h, r=r, g=g, b=b:
                    (cr.set_source_rgb(r, g, b), cr.paint()))
                da.set_hexpand(True)
                da.set_vexpand(True)
                box.append(da)
            self.set_child(box)
        else:
            rgb = STATIC_COLORS.get(pattern, (1.0, 1.0, 1.0))
            r, g, b = rgb
            da = Gtk.DrawingArea()
            da.set_draw_func(lambda w, cr, _w, _h, r=r, g=g, b=b:
                (cr.set_source_rgb(r, g, b), cr.paint()))
            self.set_child(da)

        # Press Esc or click anywhere to close
        overlay = Gtk.Overlay()
        overlay.set_child(self.get_child())
        hint = Gtk.Label(label='Press Esc or click to close')
        hint.set_css_classes(['hint'])
        hint.set_halign(Gtk.Align.CENTER)
        hint.set_valign(Gtk.Align.END)
        hint.set_margin_bottom(24)
        overlay.add_overlay(hint)

        css = Gtk.CssProvider()
        css.load_from_string('.hint { color: rgba(255,255,255,0.4); font-size: 14px; '
                             'background: rgba(0,0,0,0.3); padding: 4px 12px; '
                             'border-radius: 6px; }')
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        self.set_child(overlay)

        click2 = Gtk.GestureClick()
        click2.connect('pressed', lambda *a: app.quit())
        self.add_controller(click2)


# ── Interactive calibration window (Windows-HDR style) ─────────────────────────

class CalibrateWindow(Gtk.ApplicationWindow):
    """
    Shows a nested-box reference pattern.  The inner box represents SDR white at
    the chosen sdr_nits.  Adjust until it looks natural compared to HDR content.
    Live-applies via pkexec cosmic-hdr (polkit allow_active = no password needed).
    """

    def __init__(self, app, sdr_nits, peak_nits, gamut, bpc, gamut_mode):
        super().__init__(application=app)
        self.set_fullscreened(True)
        self._app        = app
        self._sdr_nits   = sdr_nits
        self._peak_nits  = peak_nits
        self._gamut      = gamut
        self._bpc        = bpc
        self._gamut_mode = gamut_mode
        self._pending    = False   # debounce live-apply

        key = Gtk.EventControllerKey()
        key.connect('key-pressed', self._on_key)
        self.add_controller(key)

        # Main drawing area (the calibration pattern)
        self._da = Gtk.DrawingArea()
        self._da.set_draw_func(self._draw)
        self._da.set_hexpand(True)
        self._da.set_vexpand(True)

        # Bottom control strip
        desc = Gtk.Label(
            label='Adjust until the centre square looks like a natural white '
                  'reference — not too bright, not washed out.')
        desc.set_wrap(True)
        desc.set_halign(Gtk.Align.CENTER)
        desc.set_css_classes(['caldesc'])

        self._label = Gtk.Label(label=f'SDR white brightness: {sdr_nits} nits')
        self._label.set_css_classes(['calnits'])

        self._slider = Gtk.Scale.new_with_range(
            Gtk.Orientation.HORIZONTAL, 80, 400, 1)
        self._slider.set_value(sdr_nits)
        self._slider.set_hexpand(True)
        self._slider.connect('value-changed', self._on_slider)
        self._slider.set_draw_value(False)

        btn_apply = Gtk.Button(label='Save & Apply')
        btn_apply.set_css_classes(['suggested-action'])
        btn_apply.connect('clicked', self._on_apply)

        btn_close = Gtk.Button(label='Close')
        btn_close.connect('clicked', lambda *a: app.quit())

        ctrl_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        ctrl_row.set_margin_start(32)
        ctrl_row.set_margin_end(32)
        ctrl_row.append(self._slider)
        ctrl_row.append(self._label)
        ctrl_row.append(btn_apply)
        ctrl_row.append(btn_close)

        ctrl_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        ctrl_box.set_css_classes(['calstrip'])
        ctrl_box.append(desc)
        ctrl_box.append(ctrl_row)

        overlay = Gtk.Overlay()
        overlay.set_child(self._da)

        ctrl_wrap = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        ctrl_wrap.append(ctrl_box)
        ctrl_wrap.set_valign(Gtk.Align.END)
        overlay.add_overlay(ctrl_wrap)

        self.set_child(overlay)

        # Stylesheet
        css = Gtk.CssProvider()
        css.load_from_string("""
            .calstrip {
                background: rgba(0,0,0,0.72);
                padding: 16px 8px;
            }
            .caldesc {
                color: rgba(255,255,255,0.8);
                font-size: 15px;
                margin-bottom: 4px;
            }
            .calnits {
                color: #fff;
                font-size: 16px;
                font-weight: bold;
                min-width: 180px;
            }
            scale trough {
                min-height: 6px;
            }
        """)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def _on_key(self, ctrl, keyval, *args):
        if keyval == Gdk.KEY_Escape:
            self._app.quit()
        return False

    def _on_slider(self, slider):
        v = int(slider.get_value())
        self._sdr_nits = v
        self._label.set_text(f'SDR white brightness: {v} nits')
        self._da.queue_draw()
        # Debounce live-apply: wait 300 ms after last change
        if not self._pending:
            self._pending = True
            GLib.timeout_add(300, self._live_apply)

    def _live_apply(self):
        self._pending = False
        self._do_apply()
        return False  # don't repeat

    def _on_apply(self, *_):
        self._do_apply()
        self._app.quit()

    def _do_apply(self):
        subprocess.Popen([
            'pkexec', BIN,
            '--save',
            '--sdr-nits',   str(self._sdr_nits),
            '--peak-nits',  str(self._peak_nits),
            '--gamut',      str(self._gamut),
            '--bpc',        str(self._bpc),
            '--gamut-mode', self._gamut_mode,
        ])

    def _draw(self, widget, cr, w, h):
        # Background: near-black (represents HDR dark content / shadow region)
        cr.set_source_rgb(0.02, 0.02, 0.02)
        cr.paint()

        # Outer ring: mid-gray (SDR film reference ~18% gray ≈ 0.18 relative to peak)
        ref = 0.18
        cr.set_source_rgb(ref, ref, ref)
        mw, mh = w * 0.7, h * 0.7
        cr.rectangle((w - mw) / 2, (h - mh) / 2, mw, mh)
        cr.fill()

        # Inner box: SDR reference white (1.0 in this context)
        # The display maps this through PQ metadata so actual luminance = sdr_nits
        cr.set_source_rgb(1.0, 1.0, 1.0)
        iw, ih = w * 0.28, h * 0.28
        cr.rectangle((w - iw) / 2, (h - ih) / 2, iw, ih)
        cr.fill()

        # Reference nits text overlay
        cr.set_source_rgba(0.0, 0.0, 0.0, 0.55)
        cr.select_font_face('sans-serif', 0, 0)
        cr.set_font_size(14)
        label = f'Target: {self._sdr_nits} nits SDR white'
        xb, yb, tw, th = cr.text_extents(label)[0:4]
        tx = (w - tw) / 2 - xb
        ty = (h + mh) / 2 + 28 - yb
        cr.rectangle(tx + xb - 8, ty + yb - 4, tw + 16, th + 8)
        cr.fill()
        cr.set_source_rgb(1.0, 1.0, 1.0)
        cr.move_to(tx, ty)
        cr.show_text(label)


# ── Entry point ────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]

    if '--calibrate' in args:
        # Parse flags
        def arg(flag, default):
            try:
                return args[args.index(flag) + 1]
            except (ValueError, IndexError):
                return str(default)

        sdr_nits   = int(arg('--sdr-nits',   203))
        peak_nits  = int(arg('--peak-nits',  800))
        gamut      = int(arg('--gamut',      100))
        bpc        = int(arg('--bpc',         10))
        gamut_mode = arg('--gamut-mode', 'bt2020')

        def on_activate(app):
            win = CalibrateWindow(app, sdr_nits, peak_nits, gamut, bpc, gamut_mode)
            win.present()

        app = Gtk.Application(application_id='ru.sigmachan.HdrCal.Calibrate')
        app.connect('activate', on_activate)
        app.run([])
    else:
        pattern = args[0] if args else 'white'

        def on_activate(app):
            win = StaticPatternWindow(app, pattern)
            win.present()

        app = Gtk.Application(application_id='ru.sigmachan.HdrCal.Pattern')
        app.connect('activate', on_activate)
        app.run([])


if __name__ == '__main__':
    main()
