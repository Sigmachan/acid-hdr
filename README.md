# acid-hdr

KMS/DRM color pipeline injector for Linux Wayland compositors that don't expose color management — tested on COSMIC (cosmic-comp / Smithay).

Sets a **saturation CTM** on the GPU's display engine via atomic KMS commit, giving a vivid/punchy "TV store demo" look on any connected display. Also attempts HDR10 metadata + BT2020 colorspace output.

Works by briefly switching VTs so the compositor drops DRM master, taking master, committing the color properties (which persist after release — Smithay doesn't touch them), then switching back.

## What it does

- **CTM only** — DEGAMMA/GAMMA bypassed to avoid darkening. Saturation matrix S≈1.55 with a cool blue tilt applied in the GPU color pipeline.
- **HDR10 metadata** — attempts to send SMPTE ST2086 static metadata + `BT2020_RGB` colorspace to trigger the display's HDR mode (requires `ALLOW_MODESET`; may fail depending on driver/display).
- **Persistent** — properties survive compositor repaints. Reset with `acid-hdr reset`.

## Requirements

- Linux with DRM atomic modesetting (KMS)
- libdrm
- Root (for VT switch + DRM master acquisition)
- Display on a secondary GPU / iGPU (tested: AMD RDNA2 iGPU on Ryzen 9 9950X3D → Hisense 55" A85H OLED via HDMI)

## Build

```bash
make
sudo make install
```

## Usage

```bash
sudo acid-hdr          # apply vivid color pipeline
sudo acid-hdr reset    # restore defaults
```

## Autostart (systemd)

```bash
sudo tee /etc/systemd/system/acid-hdr.service > /dev/null << 'EOF'
[Unit]
Description=acid-hdr vivid color pipeline
After=graphical.target

[Service]
Type=oneshot
ExecStart=/usr/local/bin/acid-hdr
RemainAfterExit=yes

[Install]
WantedBy=graphical.target
EOF
sudo systemctl enable --now acid-hdr.service
```

## Tuning

Edit `acid-hdr.c`:

| constant | default | effect |
|---|---|---|
| `ACID_CTM` | S=1.55, cool tilt | saturation matrix — change for more/less punch |
| `BRIGHTNESS` | 1.00 | unused with CTM-only; reserved for future LUT use |
| `CONTRAST_STR` | 0.15 | unused with CTM-only; reserved for future LUT use |

## Notes

- VT switch causes ~0.5s blank. Normal.
- HDR commit (`EINVAL`) is a driver/modeset state issue; the saturation CTM works without it.
- On compositors that expose `wlr-gamma-control-v1` or `wp-color-management-v1`, use those instead — no VT dance needed.
