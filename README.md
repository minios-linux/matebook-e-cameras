# MateBook E cameras

Camera integration for the Huawei MateBook E 2023 (DRR-WXX), developed and
tested with MiniOS, Debian 13 and Linux 6.12.111.

The integration provides two application-visible cameras:

| Camera | V4L2 device | Sensor |
| --- | --- | --- |
| MateBook Front | `/dev/video42` | Hynix HI846 |
| MateBook Rear | `/dev/video43` | Samsung S5K3L6 |

The on-demand bridge publishes 1280×720 YUYV video at approximately 30 fps.
It starts a sensor when an application opens its public device and releases
it after the last client closes. The rear camera performs an initial
contrast-based focus scan. Internal IPU6 capture channels are private to the
system service, keeping the desktop camera list at two entries.

## Packages

- `matebook-e-cameras-dkms`: IPU bridge, HI846, S5K3L6 and DW9714 driver sources.
- `matebook-e-cameras`: the system service, udev rules, and a private libcamera
  stack with the sensor gain helpers and request-cancellation fix.

Maintainer: **crims0n <crims0n@minios.dev>**, following the other MiniOS packages.

The private libcamera stack includes its own GStreamer plugin and IPA worker;
it is used by the bridge through service-local paths. libcamera upstream
version 0.7.1 is independent of this integration's Debian version.

## Use

Select `MateBook Front` or `MateBook Rear` in the application's camera settings.
For a Cheese preview:

```sh
cheese --device=/dev/video42
```

Service status and logs:

```sh
systemctl status matebook-e-cameras.service matebook-e-camera-bridge.service
journalctl -u matebook-e-camera-bridge.service
```

## Build

Build the Debian packages in Debian 13/Trixie:

```sh
dpkg-buildpackage -b -us -uc
```

The userspace binary targets amd64, matching the supported Alder Lake tablet.
DKMS builds against the installed kernel headers. Package installation enables
the services; reboot to activate the replacement kernel modules.

The root `Makefile` is the DKMS entry point and calls the kernel build system
with `M=<source>/drivers`. `drivers/Makefile` is Kbuild-only. Keeping these
separate allows DKMS to pass `KERNELRELEASE` to its outer make invocation.

## Sources and hardware status

Kernel drivers originate from Linux 6.12, with MateBook-specific ACPI,
clock, register and mode integration. `vendor/libcamera` is upstream tag
`v0.7.1` with sensor helpers and cancellation of outstanding Simple-pipeline
requests during stream shutdown.

See [the hardware record](docs/hardware-validation.md) for tested capture paths,
the manually deployed prototype and the remaining M-Pencil 3 investigation.
The experimental Goodix pen driver is preserved under `experiments/`; it is
not installed by the camera packages.
