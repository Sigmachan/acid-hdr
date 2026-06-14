# Maintainer: Kira Keller <senedato@gmail.com>
pkgbase='cosmic-hdr'
pkgname=('cosmic-hdr' 'cosmic-hdr-panel')
pkgver=0.3.0
pkgrel=1
pkgdesc='Universal KMS HDR injector for Linux with NVIDIA gaming integration'
arch=('x86_64')
url='https://github.com/Sigmachan/cosmic-hdr'
license=('GPL-3.0-only')
makedepends=('libdrm' 'rust' 'cargo' 'python')
source=(
    "cosmic-hdr-$pkgver.tar.gz::https://github.com/Sigmachan/cosmic-hdr/archive/v$pkgver.tar.gz"
    "cosmic-hdr-panel-$pkgver.tar.gz::https://github.com/Sigmachan/cosmic-hdr-panel/archive/v$pkgver.tar.gz"
)
sha256sums=('SKIP' 'SKIP')

build() {
    # C binary + hdr-game script
    cd "cosmic-hdr-$pkgver"
    make

    # Rust settings panel (separate repo)
    cd "$srcdir/cosmic-hdr-panel-$pkgver"
    cargo build --release --locked
}

package_cosmic-hdr() {
    pkgdesc='Universal KMS HDR injector — C binary, gamescope launcher, systemd service'
    depends=('libdrm' 'polkit')
    optdepends=(
        'cosmic-hdr-panel: graphical settings UI'
        'gamescope: HDR gaming on NVIDIA (hdr-game)'
        'nvibrant: digital vibrance on NVIDIA Wayland (hdr-game)'
        'python-gobject: HDR calibration overlay (hdr-cal.py)'
        'v4l-utils: HDMI-CEC control (cec-ctl)'
    )

    cd "cosmic-hdr-$pkgver"

    install -Dm755 cosmic-hdr         "$pkgdir/usr/local/bin/cosmic-hdr"
    install -Dm755 hdr-game           "$pkgdir/usr/local/bin/hdr-game"
    install -Dm644 cosmic-hdr.service "$pkgdir/usr/lib/systemd/system/cosmic-hdr.service"
    install -Dm644 cosmic-hdr.policy  "$pkgdir/usr/share/polkit-1/actions/ru.sigmachan.cosmic-hdr.policy"
    install -Dm755 hdr-cal.py         "$pkgdir/usr/local/lib/cosmic-hdr/hdr-cal.py"
}

package_cosmic-hdr-panel() {
    pkgdesc='libcosmic HDR + NVIDIA gaming settings panel for COSMIC Desktop'
    depends=('cosmic-hdr' 'libxkbcommon' 'wayland')

    cd "cosmic-hdr-panel-$pkgver"

    install -Dm755 target/release/cosmic-hdr \
        "$pkgdir/usr/local/bin/cosmic-hdr-panel"
    install -Dm644 "$srcdir/cosmic-hdr-$pkgver/cosmic-hdr-settings.desktop" \
        "$pkgdir/usr/share/applications/cosmic-hdr-panel.desktop"
}
