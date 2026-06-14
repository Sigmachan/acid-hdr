# Maintainer: Kira Keller <senedato@gmail.com>
pkgbase='cosmic-hdr'
pkgname=('cosmic-hdr' 'cosmic-hdr-panel')
pkgver=0.2.0
pkgrel=1
pkgdesc='HDR10 color pipeline for COSMIC Desktop (KMS binary + settings panel)'
arch=('x86_64')
url='https://github.com/Sigmachan/cosmic-hdr'
license=('GPL-3.0-only')
makedepends=('libdrm' 'rust' 'cargo')
source=("$pkgbase-$pkgver.tar.gz::https://github.com/Sigmachan/$pkgbase/archive/v$pkgver.tar.gz")
sha256sums=('SKIP')

build() {
    cd "$pkgbase-$pkgver"

    # C binary
    make

    # Rust panel
    cd panel
    cargo build --release --locked
}

package_cosmic-hdr() {
    pkgdesc='HDR10 KMS color pipeline injector (C binary + systemd service)'
    depends=('libdrm' 'polkit')
    optdepends=('cosmic-hdr-panel: graphical settings UI')

    cd "$pkgbase-$pkgver"

    install -Dm755 cosmic-hdr              "$pkgdir/usr/local/bin/cosmic-hdr"
    install -Dm755 hdr-game               "$pkgdir/usr/local/bin/hdr-game"
    install -Dm644 cosmic-hdr.service     "$pkgdir/usr/lib/systemd/system/cosmic-hdr.service"
    install -Dm644 cosmic-hdr.policy      "$pkgdir/usr/share/polkit-1/actions/ru.sigmachan.cosmic-hdr.policy"
    install -Dm755 hdr-cal.py             "$pkgdir/usr/local/lib/cosmic-hdr/hdr-cal.py"
}

package_cosmic-hdr-panel() {
    pkgdesc='Graphical HDR settings panel for COSMIC Desktop'
    depends=('cosmic-hdr' 'libxkbcommon' 'wayland')

    cd "$pkgbase-$pkgver/panel"

    install -Dm755 target/release/cosmic-hdr-panel \
        "$pkgdir/usr/local/bin/cosmic-hdr-panel"
    install -Dm644 ../cosmic-hdr-settings.desktop \
        "$pkgdir/usr/share/applications/cosmic-hdr-panel.desktop"
}
