//! GPU vendor detection — decides which injector pipeline is active.

use std::fs;
use std::path::Path;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GpuVendor {
    Amd,
    Intel,
    Nvidia,
    Unknown,
}

impl GpuVendor {
    pub fn label(self) -> &'static str {
        match self {
            GpuVendor::Amd => "AMD",
            GpuVendor::Intel => "Intel",
            GpuVendor::Nvidia => "NVIDIA",
            GpuVendor::Unknown => "Unknown",
        }
    }

    /// AMD/Intel get the full DEGAMMA+CTM+GAMMA atomic pipeline.
    /// NVIDIA falls back to legacy per-channel gamma ramps.
    pub fn has_full_pipeline(self) -> bool {
        matches!(self, GpuVendor::Amd | GpuVendor::Intel)
    }
}

/// Detect the primary GPU vendor.
///
/// NVIDIA is detected first via `/dev/nvidia0` (proprietary driver), then we
/// fall back to scanning DRM card PCI vendor IDs.
pub fn detect() -> GpuVendor {
    if Path::new("/dev/nvidia0").exists() {
        return GpuVendor::Nvidia;
    }
    for n in 0..8 {
        let path = format!("/sys/class/drm/card{n}/device/vendor");
        if let Ok(v) = fs::read_to_string(&path) {
            match v.trim() {
                "0x1002" => return GpuVendor::Amd,
                "0x8086" => return GpuVendor::Intel,
                "0x10de" => return GpuVendor::Nvidia,
                _ => {}
            }
        }
    }
    GpuVendor::Unknown
}
