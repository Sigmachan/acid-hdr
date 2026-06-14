//! kms-hdr-cal — native GTK4 HDR/colour calibration wizard.
//!
//! A step-by-step eyeball calibration that previews live through the kms-hdr
//! injector. Goes beyond KDE's wizard: black level, peak-via-clipping, SDR
//! white, gamut/vividness, white balance, midtone gamma, and highlight
//! roll-off — saving to `/etc/kms-hdr.conf` and optional named profiles.

mod patterns;
mod state;
mod steps;

use gtk4::prelude::*;
use gtk4::{
    Application, ApplicationWindow, Box as GtkBox, Button, DrawingArea, Label, Orientation, Switch,
};
use std::cell::Cell;
use std::rc::Rc;
use steps::STEPS;

const APP_ID: &str = "ru.sigmachan.KmsHdrCal";

fn main() {
    let app = Application::builder().application_id(APP_ID).build();
    app.connect_activate(build_ui);
    app.run();
}

fn build_ui(app: &Application) {
    let shared = state::new_shared();
    let index = Rc::new(Cell::new(0usize));

    let window = ApplicationWindow::builder()
        .application(app)
        .title("kms-hdr — Calibration")
        .default_width(1100)
        .default_height(800)
        .build();

    let root = GtkBox::new(Orientation::Vertical, 0);

    // ── header ──────────────────────────────────────────────────────────
    let header = GtkBox::new(Orientation::Vertical, 2);
    header.set_margin_top(16);
    header.set_margin_start(20);
    header.set_margin_end(20);
    let title = Label::new(None);
    title.set_xalign(0.0);
    title.add_css_class("title-1");
    let desc = Label::new(None);
    desc.set_xalign(0.0);
    desc.add_css_class("dim-label");
    desc.set_wrap(true);
    header.append(&title);
    header.append(&desc);
    root.append(&header);

    // ── test-pattern canvas ─────────────────────────────────────────────
    let canvas = DrawingArea::new();
    canvas.set_vexpand(true);
    canvas.set_hexpand(true);
    canvas.set_content_height(360);
    {
        let idx = index.clone();
        canvas.set_draw_func(move |_, cr, w, h| {
            let step = STEPS[idx.get()];
            patterns::draw(step.pattern(), cr, w as f64, h as f64);
        });
    }
    let canvas_frame = gtk4::Frame::new(None);
    canvas_frame.set_margin_start(20);
    canvas_frame.set_margin_end(20);
    canvas_frame.set_margin_top(12);
    canvas_frame.set_child(Some(&canvas));
    root.append(&canvas_frame);

    // ── controls holder (swapped per step) ──────────────────────────────
    let controls = GtkBox::new(Orientation::Vertical, 0);
    controls.set_margin_start(20);
    controls.set_margin_end(20);
    controls.set_margin_top(8);
    root.append(&controls);

    // notify: live re-apply + redraw on any control change
    let notify: Rc<dyn Fn()> = {
        let sh = shared.clone();
        let da = canvas.clone();
        Rc::new(move || {
            state::schedule_apply(&sh);
            da.queue_draw();
        })
    };

    // ── footer nav ──────────────────────────────────────────────────────
    let footer = GtkBox::new(Orientation::Horizontal, 8);
    footer.set_margin_top(8);
    footer.set_margin_bottom(16);
    footer.set_margin_start(20);
    footer.set_margin_end(20);

    let live = Switch::new();
    live.set_active(true);
    live.set_valign(gtk4::Align::Center);
    let live_lbl = Label::new(Some("Live preview"));
    footer.append(&live_lbl);
    footer.append(&live);

    let step_lbl = Label::new(None);
    step_lbl.set_hexpand(true);
    step_lbl.set_halign(gtk4::Align::Center);
    footer.append(&step_lbl);

    let cancel = Button::with_label("Reset to SDR");
    let back = Button::with_label("Back");
    let next = Button::with_label("Next");
    next.add_css_class("suggested-action");
    footer.append(&cancel);
    footer.append(&back);
    footer.append(&next);
    root.append(&footer);

    window.set_child(Some(&root));

    // ── refresh: rebuild the page for the current step ──────────────────
    let refresh: Rc<dyn Fn()> = {
        let (shared, index, notify) = (shared.clone(), index.clone(), notify.clone());
        let (title, desc, controls) = (title.clone(), desc.clone(), controls.clone());
        let (canvas, step_lbl, back, next) =
            (canvas.clone(), step_lbl.clone(), back.clone(), next.clone());
        Rc::new(move || {
            let i = index.get();
            let step = STEPS[i];
            title.set_text(step.title());
            desc.set_text(step.desc());
            while let Some(child) = controls.first_child() {
                controls.remove(&child);
            }
            controls.append(&steps::build_controls(step, &shared, notify.clone()));
            step_lbl.set_text(&format!("Step {} of {}", i + 1, STEPS.len()));
            back.set_sensitive(i > 0);
            next.set_label(if i == STEPS.len() - 1 {
                "Apply & Finish"
            } else {
                "Next"
            });
            canvas.queue_draw();
        })
    };

    // ── wire nav ────────────────────────────────────────────────────────
    {
        let (index, refresh) = (index.clone(), refresh.clone());
        back.connect_clicked(move |_| {
            let i = index.get();
            if i > 0 {
                index.set(i - 1);
                refresh();
            }
        });
    }
    {
        let (index, refresh) = (index.clone(), refresh.clone());
        let (shared, window) = (shared.clone(), window.clone());
        next.connect_clicked(move |_| {
            let i = index.get();
            if i == STEPS.len() - 1 {
                if state::commit(&shared) {
                    window.close();
                } else {
                    // surface failure inline rather than silently closing
                    eprintln!("[kms-hdr-cal] apply failed — leaving wizard open");
                }
            } else {
                index.set(i + 1);
                refresh();
            }
        });
    }
    {
        let (shared, window) = (shared.clone(), window.clone());
        cancel.connect_clicked(move |_| {
            state::revert(&shared);
            window.close();
        });
    }
    {
        let shared = shared.clone();
        live.connect_active_notify(move |s| {
            shared.borrow_mut().live = s.is_active();
            if s.is_active() {
                state::schedule_apply(&shared);
            }
        });
    }

    refresh();
    window.present();
}
