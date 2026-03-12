# Maintainer: Carl <carl@example.com>
pkgname=bbl-git
pkgver=r196.27d41fb
pkgrel=1
pkgdesc='Basic Binary Lisp — embeddable scripting language for C++ data serialization'
arch=('x86_64')
url='https://github.com/carlviedmern/bbl'
license=('MIT')
depends=('gcc-libs')
makedepends=('cmake' 'gcc')
provides=('bbl')
conflicts=('bbl')
source=()

pkgver() {
    cd "$startdir"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cmake -B "$startdir/build" -S "$startdir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build "$startdir/build" -j"$(nproc)"
}

check() {
    "$startdir/build/bbl_tests"
}

package() {
    DESTDIR="$pkgdir" cmake --install "$startdir/build"
}
