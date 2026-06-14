//! Invoke the root-owned `kms-hdr` injector via polkit.
//!
//! The polkit policy (`ru.sigmachan.kms-hdr`) is `allow_active=yes`, so an active
//! session applies without a password prompt — fine for live calibration preview.

use crate::conf::Conf;
use std::io;
use std::process::{Command, Output};

/// Binary name; resolved from PATH (installed to `$PREFIX/bin/kms-hdr`).
const INJECTOR: &str = "kms-hdr";

fn run(extra: &[&str], conf: Option<&Conf>) -> io::Result<Output> {
    let mut args: Vec<String> = vec![INJECTOR.into()];
    args.extend(extra.iter().map(|s| s.to_string()));
    if let Some(c) = conf {
        args.extend(c.to_args());
    }
    Command::new("pkexec").args(&args).output()
}

/// Apply `conf` live without persisting and without a VT switch — for preview.
pub fn apply_live(conf: &Conf) -> io::Result<Output> {
    run(&["--no-vt-switch"], Some(conf))
}

/// Apply `conf` and persist it to [`crate::conf::CONF_PATH`].
pub fn apply_and_save(conf: &Conf) -> io::Result<Output> {
    run(&["--save", "--no-vt-switch"], Some(conf))
}

/// Persist `conf` without touching the display (write-only).
pub fn save_only(conf: &Conf) -> io::Result<Output> {
    run(&["--save-only"], Some(conf))
}

/// Restore the SDR/identity pipeline.
pub fn reset() -> io::Result<Output> {
    run(&["reset"], None)
}

/// True if the command exited 0.
pub fn ok(out: &Output) -> bool {
    out.status.success()
}
