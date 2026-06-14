# cosmic-hdr

HDR10 color pipeline injector for Linux — COSMIC Desktop, KDE Plasma 6, or any compositor.

Brings HDR, wide-gamut (BT.2020 / DCI-P3 D65), and 10/12-bit output to Linux desktops via the DRM kernel API, with a GUI settings panel built on libcosmic. Works out of the box with **smart defaults** derived from your display's EDID — no manual configuration needed to get a great picture.

---

## Automatic defaults

On first run, cosmic-hdr reads your display's EDID and picks the best settings automatically:

- **Peak nits** — taken from the EDID HDR Static Metadata luminance field. Falls back to 800 nits (safe for most HDR OLED/QLED panels) if the EDID field is unset.
- **SDR white** — 203 nits (ITU-R BT.2408 reference, the broadcast standard for SDR-in-HDR). This is the value the panel's interactive calibration helps you fine-tune if needed.
- **Gamut** — BT.2020 at 100% if the display reports BT.2020 or DCI-P3 support in its colorimetry block; otherwise sRGB passthrough.
- **Bit depth** — 10 bpc if HDR10 support is detected in EDID; 8 bpc otherwise.
- **VRR** — automatically preserved (no black-screen glitch on FreeSync / G-Sync displays).

The GUI panel shows everything derived from EDID — interface version (HDMI 2.1, DP 1.4), HDR formats supported (HDR10, HLG, HDR10+, Dolby Vision), DCI-P3, DSC, HDMI-CEC availability, and peak luminance — so you can see exactly what your display reported and tune from there.

---

## What it does

| Feature | AMD / Intel | NVIDIA |
|---------|:-----------:|:------:|
| HDR10 metadata (PQ / ST 2084) | ✓ | ✓ |
| Wide gamut — BT.2020 / DCI-P3 | ✓ | — ¹ |
| 10 / 12 bpc output | ✓ | ✓ |
| VRR / FreeSync / G-Sync safe | ✓ | ✓ |
| Auto-detect display from EDID | ✓ | ✓ |

¹ NVIDIA's KMS driver does not expose the DRM CTM / DEGAMMA_LUT / GAMMA_LUT CRTC properties needed for full colour-pipeline control. On NVIDIA, cosmic-hdr falls back to a legacy 1D PQ gamma ramp via `drmModeCrtcSetGamma()` — the same technique KWin uses for Plasma 6 on NVIDIA. BT.2020 / DCI-P3 gamut expansion is silently skipped on NVIDIA.

---

## Components

| Component | Location | Description |
|-----------|----------|-------------|
| `cosmic-hdr` | `/usr/local/bin/cosmic-hdr` | KMS binary — runs as root via polkit |
| `cosmic-hdr-panel` | `/usr/local/bin/cosmic-hdr-panel` | libcosmic settings panel |
| `hdr-cal.py` | `/usr/local/lib/cosmic-hdr/hdr-cal.py` | GTK4 calibration / test-pattern overlay |
| `cosmic-hdr.service` | systemd | Applies HDR at login, resets at logout |
| `cosmic-hdr.policy` | polkit | Allows the active session to invoke the binary without a password prompt |

---

## Display detection

Both the C binary and the Rust panel auto-detect your active display by enumerating `/sys/class/drm/card*-*/edid`. No hardcoded paths — survives GPU changes, cable swaps, and multi-GPU setups. The first connector with a valid EDID (≥ 128 bytes) wins.

The panel reads the EDID and reports:

| Field | Source |
|-------|--------|
| HDR10, HLG, HDR10+, Dolby Vision | CTA-861 HDR Static Metadata block + VSVDB |
| BT.2020, DCI-P3 | CTA-861 Colorimetry block (ext_tag=5) |
| HDMI 1.4 / 2.0 / 2.1 | HDMI Licensing VSDB (OUI 0x000C03) + HDMI Forum VSDB (OUI 0xC45D00) |
| DSC | EDID HF-VSDB DSC bit + `/sys/class/drm/*/dsc_enable` sysfs |
| HDMI-CEC | `/dev/cec0` present |
| Peak nits | CTA-861 HDR Static Metadata `Maximum Luminance` |

---

## HDMI version detection

The HDMI Forum VSDB (OUI `0xC45D00`) carries the `Max TMDS Character Rate` field. Rate × 5 MHz ≥ 600 MHz → HDMI 2.1. Lower rate → HDMI 2.0. If only the HDMI Licensing VSDB (OUI `0x000C03`) is present, the display is HDMI 1.4.

---

## DCI-P3

**Does it work?** Yes, on AMD and Intel.

When you select `DCI-P3 D65` as the Target Gamut, cosmic-hdr computes and loads a 3×3 CTM that maps sRGB primaries to DCI-P3 D65 primaries into the GPU's display engine. All desktop content — not just video — is rendered with the wider colour gamut. This is the same P3 colour space used by Apple displays and modern HDR TVs.

Note: `DCI-P3 D65` uses the standard D65 white point (6504 K), matching sRGB and BT.2020. The cinema native `P3-ST 431-2` (D63, 6300 K) is intentionally not exposed — it's too warm for desktop use and only correct for cinema projection booths.

On **NVIDIA**, the gamut mode is ignored silently — the NVIDIA KMS driver does not expose CTM properties (a driver limitation). PQ tone mapping still works.

---

## HDMI-CEC

**Detection: yes. Active control: shell only (for now).**

The panel detects whether `/dev/cec0` exists (Linux kernel CEC framework, exposed by GPU drivers that support it). The badge in the Display panel shows `CEC ✓` or `CEC —`.

CEC is a separate sideband channel on the HDMI cable. It cannot toggle HDR mode — the display switches to HDR by reading the HDR10 InfoFrame embedded in the video signal, which is exactly what cosmic-hdr sets via `HDR_OUTPUT_METADATA`. CEC and HDR metadata are completely independent.

What CEC _can_ do via `cec-ctl` (from `v4l-utils`):

```bash
# Announce this PC as the active HDMI source (wakes TV, switches input)
cec-ctl --device=0 --active-source phys-addr=0.0.0.0

# Power the TV on
cec-ctl --device=0 --image-view-on

# Put the TV in standby
cec-ctl --device=0 --standby
```

CEC source announcement at HDR-enable time is on the roadmap.

---

## HDR Calibration

### Interactive calibration (`Calibrate…` button in the panel)

Opens a fullscreen GTK4 overlay with a nested-box reference pattern:

- **Dark background** — represents deep shadow / HDR dark content
- **Outer gray box** — 18% reflectance reference (standard film / TV neutral gray)
- **Inner white box** — SDR reference white at the selected brightness

Drag the slider. The pipeline applies live (via `pkexec`, no password prompt — polkit `allow_active = yes`). The target: the inner white box should look naturally bright — clearly whiter than the 18% gray, but not blinding. A good starting point for HDR OLED is 203 nits (ITU-R BT.2408).

Press **Save & Apply** to commit the value. Press **Close** or **Esc** to discard.

### Test patterns

| Pattern | Use |
|---------|-----|
| Black | Verify black crush / near-black clipping |
| 5% Gray | EOTF accuracy near black (should be barely visible, not crushed to black) |
| 50% Gray | Mid-tone reference |
| White | Check HDR white clip / overexposure |
| Red / Green / Blue | Gamut primaries — check after CTM / gamut changes |
| SDR\|HDR Split | Left = 20% SDR gray, right = full HDR white; compare tone-mapping |

Click anywhere or press **Esc** to close a test pattern.

---

## Installation

### From source

```bash
# C binary + service + polkit + calibration tool
cd cosmic-hdr
make
sudo make install
sudo make enable          # enables and starts cosmic-hdr.service

# Rust panel
cd ../cosmic-hdr-panel
cargo build --release
sudo install -Dm755 target/release/cosmic-hdr-panel /usr/local/bin/cosmic-hdr-panel
```

**Dependencies:** `libdrm`, `python-gobject` (GTK4 Python bindings, for `hdr-cal.py`), `polkit`, `systemd`.

### Arch Linux (PKGBUILD)

```bash
# Inside the cosmic-hdr directory
makepkg -si
```

---

## Configuration

Written to `/etc/cosmic-hdr.conf` by `cosmic-hdr --save ...` (runs as root via polkit):

```ini
SDR_NITS=203
PEAK_NITS=800
GAMUT=100
MAX_BPC=10
GAMUT_MODE=bt2020
```

| Key | Default | Description |
|-----|---------|-------------|
| `SDR_NITS` | 203 | SDR reference white in HDR mode (nits). ITU-R BT.2408 broadcast reference. |
| `PEAK_NITS` | 800 | Display peak luminance for HDR10 metadata. Match to your display's spec. |
| `GAMUT` | 100 | Gamut expansion blend 0–100%. 0 = sRGB passthrough, 100 = full target. |
| `MAX_BPC` | 10 | Bit depth requested via `max_requested_bpc` DRM property. |
| `GAMUT_MODE` | bt2020 | Target colour space: `bt2020`, `dci-p3`, or `srgb`. |

---

## CLI reference

```
cosmic-hdr [OPTIONS] [COMMAND]

Commands:
  reset                     Reset to SDR (remove HDR metadata, restore gamma)

Options:
  --card <path>             DRM device (default: auto-detected from sysfs EDID)
  --connector <name>        Connector name, e.g. HDMI-A-2 (default: auto-detected)
  --sdr-nits <n>            SDR reference white, nits (default: config → 203)
  --peak-nits <n>           Display peak, nits (default: config → 800)
  --gamut <0-100>           Gamut expansion blend % (default: 100)
  --bpc <8|10|12>           Max requested bit depth (default: 10)
  --gamut-mode <mode>       bt2020 | dci-p3 | srgb (default: bt2020)
  --save                    Write /etc/cosmic-hdr.conf before applying
  --help                    Show this help
```

---

## VRR / FreeSync / G-Sync compatibility

Applying HDR via a full modeset (`DRM_MODE_ATOMIC_ALLOW_MODESET`) while VRR is active causes a brief black flash as the display resyncs timing. cosmic-hdr avoids this:

1. Reads the current `vrr_enabled` property value before committing.
2. Disables VRR, applies the HDR pipeline.
3. Restores `vrr_enabled` to its original value.

Additionally, a non-blocking (`DRM_MODE_ATOMIC_NONBLOCK`) commit is attempted first for the HDR metadata properties, falling back to `ALLOW_MODESET` only if that fails — avoiding the resync entirely when the compositor is idle.

---

## License

GPL-3.0-only.

Maintainer: Kira Keller \<senedato@gmail.com\>
