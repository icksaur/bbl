# Maintainer: Carl <carl@example.com>
pkgname=bbl-git
pkgver=r127.e6d5d75
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
    cmake -B build -S "$startdir" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j"$(nproc)"
}

check() {
    ./build/bbl_tests
}

package() {
    DESTDIR="$pkgdir" cmake --install build
}
