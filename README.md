# OpenRGB 1.0 – Stable fork with Aura USB and startup-profile fixes

A patched build of [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) **1.0** for people who stayed on 1.0 (or went back to it) and have one of these problems:

- An ASUS Aura USB motherboard controller is sometimes not detected, or OpenRGB hangs during detection.
- Manually set ARGB zone sizes (LED counts) are lost and must be re-entered.
- The startup profile (`--profile`) does not apply at boot; colors only work after pressing **Rescan**.
- A saved profile silently skips a device (for example an ASUS ROG Azoth keyboard) even though it is in the profile.
- Running OpenRGB as a system service under a normal user, before that user logs in.

> Not affiliated with the official OpenRGB project. All changes are on top of the `release_1.0` tag. The original upstream README is kept as [README-upstream.md](README-upstream.md).

## What is fixed

### ASUS Aura USB (motherboard / addressable controllers)
- Fixed a use-after-free: a failed config-table read deleted the controller while the detector kept using it.
- Requests now drain stale reports, use a read timeout, and only accept the reply they asked for. This stops hangs and crossed replies (for example, when two OpenRGB instances run at once).
- The firmware version string is always initialized. Before, it could contain random memory, which made saved data stop matching the device.
- If the config table cannot be read, the device is skipped cleanly instead of being registered with an empty layout.

### Profiles and saved zone sizes
- Saved data still matches a device when only its firmware/version string changed, as long as the match is unambiguous.
- Saved data still matches a device when only its serial number changed, as long as the match is unambiguous. Some devices, such as the ASUS ROG Azoth, report a serial number with trailing garbage bytes that change between reads.
- Modes are restored by name instead of by position, so a profile still applies after the mode list changes.
- Saving the configuration replaces stale duplicate entries for the same device.
- Loading a profile logs which devices it did not apply to, and returns a failure when it matched nothing.

### System service started before login
On Linux, HID devices are normally only granted to the user with an active local session (udev `uaccess`). A service running as that user starts **before** login, so every USB device is denied and the profile only reaches I2C devices such as RAM and the GPU.

This fork remembers devices that returned *Permission denied* and checks them every 5 seconds. Once access is granted (you log in), it rescans and re-applies the active profile automatically. No extra udev rules are needed.

### Always-on debug log
Writes `~/.config/OpenRGB/OpenRGB-debug.log` regardless of log level. It records the user and session state, the permissions of every `/dev/hidraw*` node, detection results, Aura USB traffic, and how each profile was applied, including why a device did not match. The file rotates to `.old` at 10 MB.

## Install (Arch / CachyOS / Arch-based)

This builds a package named `openrgb` (1.0-3) that replaces the distro package. The installed udev rules, systemd service and module files are copied from your current `openrgb` package, so it must already be installed.

```bash
mkdir -pv ~/build/openrgb-fix && cd ~/build/openrgb-fix && \
cp -v /usr/lib/udev/rules.d/60-openrgb.rules . && \
cp -v /usr/lib/modules-load.d/openrgb.conf openrgb-modules.conf && \
cp -v /usr/lib/systemd/system/openrgb.service . && \
cp -v /usr/lib/tmpfiles.d/openrgb.conf openrgb-tmpfiles.conf && \
cat > PKGBUILD <<'PKG'
pkgname=openrgb
pkgver=1.0
pkgrel=3
pkgdesc="OpenRGB 1.0 with Aura USB and profile fixes"
arch=(x86_64)
url="https://github.com/vdurzo/OpenRGB-Stable-CLI"
license=(GPL-2.0-only)
depends=(glibc libgcc libstdc++ qt6-base libusb hidapi mbedtls3 hicolor-icon-theme)
makedepends=(git qt6-tools)
source=("openrgb-src::git+https://github.com/vdurzo/OpenRGB-Stable-CLI.git#branch=fix/aura-profiles-1.0"
        60-openrgb.rules openrgb-modules.conf openrgb.service openrgb-tmpfiles.conf)
sha256sums=(SKIP SKIP SKIP SKIP SKIP)

build() {
  mkdir -p build && cd build
  qmake6 ../openrgb-src/OpenRGB.pro PREFIX=/usr
  make -j"$(nproc)"
}

package() {
  cd build
  make INSTALL_ROOT="$pkgdir" install
  install -Dm644 "$srcdir/60-openrgb.rules"      "$pkgdir/usr/lib/udev/rules.d/60-openrgb.rules"
  install -Dm644 "$srcdir/openrgb-modules.conf"  "$pkgdir/usr/lib/modules-load.d/openrgb.conf"
  install -Dm644 "$srcdir/openrgb.service"       "$pkgdir/usr/lib/systemd/system/openrgb.service"
  install -Dm644 "$srcdir/openrgb-tmpfiles.conf" "$pkgdir/usr/lib/tmpfiles.d/openrgb.conf"
}
PKG
makepkg -si
```

To update later, run `makepkg -sif` again in the same folder.

To stop pacman from replacing it on the next system update, add `openrgb` to `IgnorePkg` in `/etc/pacman.conf`.

## Build from source (any Linux)

```bash
git clone -b fix/aura-profiles-1.0 https://github.com/vdurzo/OpenRGB-Stable-CLI.git
cd OpenRGB-Stable-CLI && mkdir build && cd build
qmake6 ../OpenRGB.pro PREFIX=/usr && make -j"$(nproc)"
```

Requires Qt 6, hidapi, libusb and mbedtls 3.x.

## Tested on
- CachyOS, KDE Plasma 6, ASUS TUF GAMING X670E-PLUS WIFI (Aura USB controller `0b05:19af`, 3 ARGB headers)
- ASUS ROG Azoth keyboard (wired `0b05:1a83` and 2.4 GHz `0b05:1a85`), Logitech G502, ENE DRAM, ASUS ROG Strix RTX 2080 Ti
- OpenRGB running as a systemd service with `User=` set to a regular user and `--server --profile <name>`

Other Aura USB devices use the same code paths, but I have not tested them.

## License
GPL-2.0-or-later, same as upstream OpenRGB.
