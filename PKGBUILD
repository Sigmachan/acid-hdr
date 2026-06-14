# Maintainer: Kira Keller <senedato@gmail.com>
pkgbase='kms-hdr'
pkgname=('kms-hdr' 'kms-hdr-panel')
pkgver=0.3.0
pkgrel=1
pkgdesc='Universal KMS HDR injector for Linux — compositor-agnostic, NVIDIA gaming integration'
arch=('x86_64')
url='https://github.com/Sigmachan/kms-hdr'
license=('GPL-3.0-only')
makedepends=('libdrm' 'rust' 'cargo' 'python')
source=(
    "kms-hdr-$pkgver.tar.gz::https://github.com/Sigmachan/kms-hdr/archive/v$pkgver.tar.gz"
    "kms-hdr-panel-$pkgver.tar.gz::https://github.com/Sigmachan/kms-hdr-panel/archive/v$pkgver.tar.gz"
)
sha256sums=('SKIP' 'SKIP')

build() {
    cd "kms-hdr-$pkgver"
    make

    cd "$srcdir/kms-hdr-panel-$pkgver"
    cargo build --release --locked
}

package_kms-hdr() {
    pkgdesc='Universal KMS HDR injector — C binary, Gamescope launcher, systemd service'
    depends=('libdrm' 'polkit')
    optdepends=(
        'kms-hdr-panel: graphical settings UI'
        'gamescope: HDR gaming on NVIDIA (hdr-game)'
        'nvibrant: digital vibrance on NVIDIA Wayland (hdr-game)'
        'python-gobject: HDR calibration overlay (hdr-cal.py)'
        'v4l-utils: HDMI-CEC control (cec-ctl)'
    )

    cd "kms-hdr-$pkgver"

    install -Dm755 kms-hdr          "$pkgdir/usr/local/bin/kms-hdr"
    install -Dm755 hdr-game         "$pkgdir/usr/local/bin/hdr-game"
    install -Dm644 kms-hdr.service  "$pkgdir/usr/lib/systemd/system/kms-hdr.service"
    install -Dm644 kms-hdr.policy   "$pkgdir/usr/share/polkit-1/actions/ru.sigmachan.kms-hdr.policy"
    install -Dm755 hdr-cal.py       "$pkgdir/usr/local/lib/kms-hdr/hdr-cal.py"
}

package_kms-hdr-panel() {
    pkgdesc='libcosmic HDR + NVIDIA gaming settings panel'
    depends=('kms-hdr' 'libxkbcommon' 'wayland')

    cd "kms-hdr-panel-$pkgver"

    install -Dm755 target/release/kms-hdr-panel \
        "$pkgdir/usr/local/bin/kms-hdr-panel"
    install -Dm644 "$srcdir/kms-hdr-$pkgver/cosmic-hdr-settings.desktop" \
        "$pkgdir/usr/share/applications/kms-hdr-panel.desktop"
}
