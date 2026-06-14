//! Named calibration profiles, stored under `$XDG_CONFIG_HOME/kms-hdr/profiles`.
//!
//! Each profile is a [`Conf`] serialized in the same `KEY=VALUE` format as the
//! system config, so a profile can be applied verbatim by the injector.

use crate::conf::Conf;
use std::fs;
use std::path::PathBuf;

/// `~/.config/kms-hdr/profiles` (honors `XDG_CONFIG_HOME`).
pub fn profiles_dir() -> PathBuf {
    let base = std::env::var_os("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .unwrap_or_else(|| PathBuf::from("/tmp"));
    base.join("kms-hdr").join("profiles")
}

fn sanitize(name: &str) -> String {
    name.chars()
        .map(|c| if c.is_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .collect()
}

fn profile_path(name: &str) -> PathBuf {
    profiles_dir().join(format!("{}.conf", sanitize(name)))
}

/// List saved profile names (without extension), sorted.
pub fn list() -> Vec<String> {
    let mut names = Vec::new();
    if let Ok(entries) = fs::read_dir(profiles_dir()) {
        for e in entries.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) == Some("conf") {
                if let Some(stem) = p.file_stem().and_then(|s| s.to_str()) {
                    names.push(stem.to_string());
                }
            }
        }
    }
    names.sort();
    names
}

/// Save `conf` under `name`, creating the profiles dir if needed.
pub fn save(name: &str, conf: &Conf) -> std::io::Result<()> {
    let dir = profiles_dir();
    fs::create_dir_all(&dir)?;
    fs::write(profile_path(name), conf.to_conf_string())
}

/// Load a profile by name, if it exists.
pub fn load(name: &str) -> Option<Conf> {
    let path = profile_path(name);
    if path.exists() {
        Some(Conf::load_from(path))
    } else {
        None
    }
}

/// Delete a profile by name.
pub fn delete(name: &str) -> std::io::Result<()> {
    fs::remove_file(profile_path(name))
}
