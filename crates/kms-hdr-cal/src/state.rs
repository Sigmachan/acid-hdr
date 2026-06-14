//! Shared wizard state and debounced live-preview application.

use kms_hdr_core::{apply, conf::Conf, edid::DisplayInfo, gpu::GpuVendor};
use std::cell::RefCell;
use std::rc::Rc;
use std::time::Duration;

pub struct State {
    pub conf: Conf,
    pub display: Option<DisplayInfo>,
    pub gpu: GpuVendor,
    /// Live preview pushes every change to the display (debounced).
    pub live: bool,
    /// Debounce generation counter.
    pub gen: u64,
}

pub type Shared = Rc<RefCell<State>>;

/// Build initial state from the running system: EDID, GPU, current conf.
pub fn new_shared() -> Shared {
    let display = kms_hdr_core::edid::detect();
    let gpu = kms_hdr_core::gpu::detect();
    let mut conf = Conf::load();

    // Seed peak luminance from EDID if the conf is still at its default.
    if let Some(d) = &display {
        if let Some(p) = d.peak_nits {
            if conf.peak_nits == Conf::default().peak_nits {
                conf.peak_nits = p as i32;
            }
        }
        if d.is_oled {
            conf.force_oled = true;
        }
    }

    Rc::new(RefCell::new(State {
        conf,
        display,
        gpu,
        live: true,
        gen: 0,
    }))
}

/// Schedule a debounced live apply. Coalesces rapid slider drags into one
/// `pkexec kms-hdr` call ~250ms after the last change, off the UI thread.
pub fn schedule_apply(shared: &Shared) {
    if !shared.borrow().live {
        return;
    }
    let g = {
        let mut s = shared.borrow_mut();
        s.gen = s.gen.wrapping_add(1);
        s.gen
    };
    let sh = shared.clone();
    gtk4::glib::timeout_add_local_once(Duration::from_millis(250), move || {
        if sh.borrow().gen != g {
            return; // superseded by a newer change
        }
        let conf = sh.borrow().conf.clone();
        std::thread::spawn(move || {
            if let Err(e) = apply::apply_live(&conf) {
                eprintln!("[kms-hdr-cal] live apply failed: {e}");
            }
        });
    });
}

/// Persist + apply the final configuration.
pub fn commit(shared: &Shared) -> bool {
    let conf = shared.borrow().conf.clone();
    match apply::apply_and_save(&conf) {
        Ok(out) => apply::ok(&out),
        Err(e) => {
            eprintln!("[kms-hdr-cal] commit failed: {e}");
            false
        }
    }
}

/// Restore SDR and drop live preview.
pub fn revert(shared: &Shared) {
    if let Err(e) = apply::reset() {
        eprintln!("[kms-hdr-cal] reset failed: {e}");
    }
    shared.borrow_mut().live = false;
}
