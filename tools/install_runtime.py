#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Stage the private runtime and DKMS sources for dh_install."""
from pathlib import Path
import shutil
import sys

build = Path(sys.argv[1])
destination = Path(sys.argv[2])
multiarch = sys.argv[3]
version = sys.argv[4]
private = destination / "usr/lib" / multiarch / "matebook-e-cameras"


def install(source, target, executable=False):
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o755 if executable else 0o644)


for name, source in (
    ("libcamera.so.0.7.1", build / "src/libcamera/libcamera.so.0.7.1"),
    ("libcamera-base.so.0.7.1", build / "src/libcamera/base/libcamera-base.so.0.7.1"),
):
    install(source, private / "lib" / name)
    (private / "lib" / name.removesuffix(".1")).symlink_to(name)

install(build / "src/gstreamer/libgstlibcamera.so",
        private / "gstreamer-1.0/libgstlibcamera.so")
install(build / "src/ipa/simple/ipa_soft_simple.so",
        private / "ipa/ipa_soft_simple.so")
install(build / "src/libcamera/proxy/worker/soft_ipa_proxy",
        destination / "usr/libexec/matebook-e-cameras/soft_ipa_proxy", True)
install("src/matebook_camera_bridge.py",
        destination / "usr/libexec/matebook-e-cameras/matebook_camera_bridge.py", True)
install("vendor/libcamera/src/ipa/simple/data/uncalibrated.yaml",
        destination / "usr/share/matebook-e-cameras/ipa/simple/uncalibrated.yaml")
for license_file in Path("vendor/libcamera/LICENSES").iterdir():
    install(license_file, destination / "usr/share/matebook-e-cameras/licenses" / license_file.name)

install("integration/matebook-e-cameras.conf",
        destination / "etc/modprobe.d/matebook-e-cameras.conf")
for rule in Path("integration").glob("*.rules"):
    install(rule, destination / "usr/lib/udev/rules.d" / rule.name)

drivers = destination / "usr/src" / f"matebook-e-cameras-{version}" / "drivers"
for source in Path("drivers").iterdir():
    if source.suffix in (".c", ".h") or source.name == "Makefile":
        install(source, drivers / source.name)

install("integration/matebook-e-cameras.service",
        "debian/matebook-e-cameras.matebook-e-cameras.service")
service = Path("integration/matebook-e-camera-bridge.service.in").read_text()
Path("debian/matebook-e-cameras.matebook-e-camera-bridge.service").write_text(
    service.replace("@LIBDIR@", f"/usr/lib/{multiarch}"))
