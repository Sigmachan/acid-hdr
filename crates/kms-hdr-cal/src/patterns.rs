//! Cairo test-pattern rendering for each calibration step.
//!
//! Patterns are drawn in plain SDR sRGB; because the display is already in HDR
//! mode (the injector is live), they show *through* the colour pipeline — the
//! same eyeball-calibration trick KDE and the Windows HDR wizard use.

use gtk4::cairo::Context;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Pattern {
    Info,
    BlackLevel,
    PeakClip,
    WhiteRef,
    Gamut,
    WhiteBalance,
    Gamma,
    Tonemap,
    Summary,
}

/// Paint `pattern` filling a `w`×`h` area.
pub fn draw(p: Pattern, cr: &Context, w: f64, h: f64) {
    // Neutral dark backdrop for every pattern.
    cr.set_source_rgb(0.04, 0.04, 0.05);
    let _ = cr.paint();
    match p {
        Pattern::Info | Pattern::Summary => draw_logo(cr, w, h),
        Pattern::BlackLevel => draw_black_level(cr, w, h),
        Pattern::PeakClip => draw_peak_clip(cr, w, h),
        Pattern::WhiteRef => draw_white_ref(cr, w, h),
        Pattern::Gamut => draw_gamut(cr, w, h),
        Pattern::WhiteBalance => draw_white_balance(cr, w, h),
        Pattern::Gamma => draw_gamma(cr, w, h),
        Pattern::Tonemap => draw_tonemap(cr, w, h),
    }
}

fn centered_text(cr: &Context, w: f64, y: f64, size: f64, text: &str) {
    cr.set_font_size(size);
    let ext = cr.text_extents(text).unwrap();
    cr.move_to(w / 2.0 - ext.width() / 2.0, y);
    let _ = cr.show_text(text);
}

fn draw_logo(cr: &Context, w: f64, h: f64) {
    cr.set_source_rgb(0.55, 0.45, 0.85); // mauve, Catppuccin-ish
    centered_text(cr, w, h / 2.0 - 10.0, 34.0, "kms-hdr");
    cr.set_source_rgb(0.5, 0.5, 0.55);
    centered_text(cr, w, h / 2.0 + 28.0, 16.0, "HDR & wide-gamut calibration");
}

/// PLUGE-style near-black patches to set the shadow floor.
fn draw_black_level(cr: &Context, w: f64, h: f64) {
    let n = 6;
    let pw = w / (n as f64 + 2.0);
    let y = h / 2.0 - pw / 2.0;
    for i in 0..n {
        // 0,1,2,3,4,5 over 255 — should be just barely distinguishable
        let v = (i as f64) / 255.0;
        cr.set_source_rgb(v, v, v);
        cr.rectangle(pw * (i as f64 + 1.0), y, pw * 0.85, pw);
        let _ = cr.fill();
    }
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.85, 15.0, "Raise black-lift until the leftmost bars separate from pure black");
}

/// Bright field with a slightly-brighter detail — peak-luminance clipping test.
fn draw_peak_clip(cr: &Context, w: f64, h: f64) {
    // near-peak background
    cr.set_source_rgb(0.92, 0.92, 0.92);
    cr.rectangle(w * 0.15, h * 0.15, w * 0.7, h * 0.6);
    let _ = cr.fill();
    // brighter cross detail that should remain visible until clipping
    cr.set_source_rgb(1.0, 1.0, 1.0);
    let cx = w / 2.0;
    let cy = h * 0.15 + h * 0.3;
    cr.rectangle(cx - 4.0, h * 0.18, 8.0, h * 0.54);
    let _ = cr.fill();
    cr.rectangle(w * 0.18, cy - 4.0, w * 0.64, 8.0);
    let _ = cr.fill();
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.9, 15.0, "Lower peak-nits until the white cross JUST disappears into the field");
}

/// Large SDR-white rectangle to set the reference brightness.
fn draw_white_ref(cr: &Context, w: f64, h: f64) {
    cr.set_source_rgb(1.0, 1.0, 1.0);
    cr.rectangle(w * 0.2, h * 0.18, w * 0.6, h * 0.55);
    let _ = cr.fill();
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.85, 15.0, "Set SDR white so this panel matches comfortable paper-white");
}

/// Saturated primary/secondary swatches for gamut + vividness judgement.
fn draw_gamut(cr: &Context, w: f64, h: f64) {
    let cols = [
        (0.90, 0.10, 0.10),
        (0.10, 0.75, 0.20),
        (0.15, 0.30, 0.95),
        (0.10, 0.80, 0.85),
        (0.90, 0.20, 0.85),
        (0.95, 0.85, 0.15),
        (0.85, 0.65, 0.50), // skin tone — the vividness gut-check
    ];
    let n = cols.len();
    let sw = w / (n as f64 + 1.0);
    let y = h * 0.25;
    for (i, (r, g, b)) in cols.iter().enumerate() {
        cr.set_source_rgb(*r, *g, *b);
        cr.rectangle(sw * (i as f64 + 0.5), y, sw * 0.9, h * 0.4);
        let _ = cr.fill();
    }
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.8, 15.0, "Push saturation/gamut until colours pop without the skin patch going neon");
}

/// Large neutral gray field — reveals colour casts for white-balance.
fn draw_white_balance(cr: &Context, w: f64, h: f64) {
    cr.set_source_rgb(0.5, 0.5, 0.5);
    cr.rectangle(w * 0.12, h * 0.15, w * 0.76, h * 0.6);
    let _ = cr.fill();
    // small reference patches at the corners
    for (gx, gy) in [(0.06, 0.06), (0.88, 0.06), (0.06, 0.82), (0.88, 0.82)] {
        cr.set_source_rgb(0.75, 0.75, 0.75);
        cr.rectangle(w * gx, h * gy, w * 0.06, h * 0.1);
        let _ = cr.fill();
    }
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.9, 15.0, "Adjust temperature/tint until the gray field looks truly neutral (no cast)");
}

/// Smooth luminance ramp + step bars for gamma/midtone tuning.
fn draw_gamma(cr: &Context, w: f64, h: f64) {
    let steps = 256;
    let bw = (w * 0.8) / steps as f64;
    let x0 = w * 0.1;
    for i in 0..steps {
        let v = i as f64 / (steps - 1) as f64;
        cr.set_source_rgb(v, v, v);
        cr.rectangle(x0 + bw * i as f64, h * 0.2, bw + 1.0, h * 0.25);
        let _ = cr.fill();
    }
    // discrete step bars below for banding/gamma check
    let n = 11;
    let sw = (w * 0.8) / n as f64;
    for i in 0..n {
        let v = i as f64 / (n - 1) as f64;
        cr.set_source_rgb(v, v, v);
        cr.rectangle(x0 + sw * i as f64, h * 0.5, sw, h * 0.2);
        let _ = cr.fill();
    }
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.85, 15.0, "Tune midtone gamma for an even ramp — no crushed shadows or washed mids");
}

/// Highlight gradient to evaluate roll-off vs hard clip near peak.
fn draw_tonemap(cr: &Context, w: f64, h: f64) {
    let steps = 256;
    let bw = (w * 0.8) / steps as f64;
    let x0 = w * 0.1;
    // upper 25% of the range, stretched — where clipping shows
    for i in 0..steps {
        let v = 0.75 + 0.25 * (i as f64 / (steps - 1) as f64);
        cr.set_source_rgb(v, v, v);
        cr.rectangle(x0 + bw * i as f64, h * 0.25, bw + 1.0, h * 0.4);
        let _ = cr.fill();
    }
    cr.set_source_rgb(0.5, 0.5, 0.5);
    centered_text(cr, w, h * 0.8, 15.0, "Roll-off keeps detail in the brightest gradient steps instead of clipping to flat white");
}
