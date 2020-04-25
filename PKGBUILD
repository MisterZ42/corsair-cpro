# Maintainer: Marius Zachmann
# Contributor: Marius Zachmann

_pkgbase=corsair-cpro
pkgname=corsair-cpro-dkms
pkgver=1
pkgrel=1
pkgdesc="Module for Corsair Commander Pro"
arch=('x86_64')
url=""
license=('GPL2')
depends=('make' 'gcc' 'linux' 'dkms')
makedepends=('git' 'linux-headers')
install=${pkgname}.install
source=('~/builds/corsair-cpro/corsair-cpro.c'
        '~/builds/corsair-cpro/Makefile'
        '~/builds/corsair-cpro/10-corsair-cpro.rules'
        '~/builds/corsair-cpro/corsair-cpro.conf'
        '~/builds/corsair-cpro/dkms.conf')
md5sums=('SKIP'
         'SKIP'
         'SKIP'
         'SKIP'
         'SKIP')

package() {

  install -Dm644 dkms.conf "${pkgdir}/usr/src/${_pkgbase}-${pkgver}/dkms.conf"



  # Set name and version
  sed -e "s/@_PKGBASE@/${_pkgbase}/" \
      -e "s/@PKGVER@/${pkgver}/" \
      -i "${pkgdir}/usr/src/${_pkgbase}-${pkgver}/dkms.conf"

  # Copy sources (including Makefile)
  cp -rL ${srcdir}/{corsair-cpro.c,Makefile} "${pkgdir}/usr/src/${_pkgbase}-${pkgver}/"

  # Create Udev-Rule and Module Start
  mkdir -p "${pkgdir}/etc/modules-load.d"
  mkdir -p "${pkgdir}/etc/udev/rules.d"
  cp -rL ${srcdir}/10-corsair-cpro.rules "${pkgdir}/etc/udev/rules.d/"
  cp -rL ${srcdir}/corsair-cpro.conf "${pkgdir}/etc/modules-load.d/"

}
