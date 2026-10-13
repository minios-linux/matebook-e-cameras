#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Expose IPU6 cameras as V4L2 devices; acquire sensors only while a client uses them."""
import errno
import fcntl
import os
from pathlib import Path
import signal
import socket
import struct
import time

import gi
gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

Gst.init(None)
WIDTH, HEIGHT = 1280, 720
VIDIOC_S_FMT = 0xC0D05605


def readers(proc_root=Path("/proc"), own_pid=None):
    if own_pid is None:
        own_pid = os.getpid()
    active = set()
    for proc in Path(proc_root).iterdir():
        if not proc.name.isdecimal() or int(proc.name) == own_pid:
            continue
        try:
            descriptors = tuple((proc / "fd").iterdir())
        except OSError:
            continue
        for descriptor in descriptors:
            try:
                target = os.readlink(descriptor)
            except OSError:
                continue
            if target in ("/dev/video42", "/dev/video43"):
                active.add(target)
    return active


def contrast_score(data, width, height):
    total = contrast = 0
    for y in range(height // 3, height * 2 // 3, 12):
        for x in range(width // 3, width * 2 // 3, 12):
            offset = (y * width + x) * 2
            luminance = data[offset]
            total += luminance
            contrast += abs(luminance - data[offset + 8])
            contrast += abs(luminance - data[offset + width * 2 * 4])
    return contrast / max(total, 1)


def notify_ready():
    if "NOTIFY_SOCKET" not in os.environ:
        return
    address = os.environ["NOTIFY_SOCKET"]
    if address.startswith("@"):
        address = "\0" + address[1:]
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notify:
        notify.connect(address)
        notify.sendall(b"READY=1")


class Camera:
    def __init__(self, device, camera_name):
        self.device = device
        self.camera_name = camera_name
        self.fd = os.open(device, os.O_RDWR | os.O_NONBLOCK)
        fmt = bytearray(208)
        struct.pack_into("=I", fmt, 0, 2)  # V4L2_BUF_TYPE_VIDEO_OUTPUT
        struct.pack_into("=8I", fmt, 8, WIDTH, HEIGHT,
                         int.from_bytes(b"YUYV", "little"), 1,
                         WIDTH * 2, WIDTH * HEIGHT * 2, 1, 0)
        fcntl.ioctl(self.fd, VIDIOC_S_FMT, fmt)
        self.blank = bytes((16, 128, 16, 128)) * (WIDTH * HEIGHT // 2)
        os.write(self.fd, self.blank)
        self.pipeline = None
        self.running = False
        self.last_reader = 0
        self.retry_at = 0
        self.lens_fd = None
        self.focus_steps = []
        self.focus_best = (0.0, 256)
        self.focus_score = 0.0
        self.focus_after = 0.0
        print(f"{device}: ready (standby)", flush=True)

    def sample(self, sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.EOS
        buffer = sample.get_buffer()
        ok, mapped = buffer.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.ERROR
        try:
            os.write(self.fd, mapped.data)
            if self.lens_fd is not None and self.focus_steps:
                self.focus_score = contrast_score(mapped.data, WIDTH, HEIGHT)
        except OSError as error:
            if error.errno not in (errno.EAGAIN, errno.EINTR):
                print(f"{self.device}: frame write failed: {error}", flush=True)
        finally:
            buffer.unmap(mapped)
        return Gst.FlowReturn.OK

    def message(self, bus, message):
        if message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            print(f"{self.device}: {error}; {debug}", flush=True)
            GLib.idle_add(self.failed)

    def failed(self):
        self.stop()
        self.retry_at = time.monotonic() + 5
        return GLib.SOURCE_REMOVE

    def start(self):
        if self.pipeline is None:
            self.pipeline = Gst.parse_launch(
                'libcamerasrc name=source ! '
                f'video/x-raw,width={WIDTH},height={HEIGHT} ! '
                'queue max-size-buffers=2 leaky=downstream ! videoconvert ! '
                'video/x-raw,format=YUY2 ! '
                'appsink name=output emit-signals=true max-buffers=2 drop=true sync=false')
            self.pipeline.get_by_name("source").set_property("camera-name", self.camera_name)
            self.pipeline.get_by_name("output").connect("new-sample", self.sample)
            bus = self.pipeline.get_bus()
            bus.add_signal_watch()
            bus.connect("message", self.message)
        self.pipeline.set_state(Gst.State.PLAYING)
        self.running = True
        if self.camera_name.endswith("LNK1"):
            for node in Path("/sys/class/video4linux").glob("v4l-subdev*"):
                try:
                    if not (node / "name").read_text().startswith("dw9714"):
                        continue
                    self.lens_fd = os.open("/dev/" + node.name, os.O_RDWR | os.O_NONBLOCK)
                    self.focus_steps = list(range(0, 897, 128)) + [None]
                    self.focus_best = (0.0, 256)
                    if hasattr(self, "focus_position"):
                        del self.focus_position
                    self.focus_after = time.monotonic() + 3
                    break
                except OSError:
                    continue
        print(f"{self.device}: capturing", flush=True)

    def stop(self):
        if self.pipeline is not None and self.running:
            # Keep CameraManager alive while the software ISP drains IPC replies.
            # READY stops capture and frees capture buffers without destroying it.
            self.pipeline.set_state(Gst.State.READY)
            self.running = False
            if self.lens_fd is not None:
                os.close(self.lens_fd)
                self.lens_fd = None
            self.focus_steps = []
            os.write(self.fd, self.blank)
            print(f"{self.device}: standby", flush=True)

    def focus(self, now):
        if self.lens_fd is None or not self.focus_steps or now < self.focus_after:
            return
        position = self.focus_steps.pop(0)
        if hasattr(self, "focus_position") and self.focus_score > self.focus_best[0]:
            self.focus_best = (self.focus_score, self.focus_position)
        if position is None:
            position = self.focus_best[1]
            print(f"{self.device}: focus position {position}", flush=True)
        try:
            fcntl.ioctl(self.lens_fd, 0xC008561C, struct.pack("=Ii", 0x009A090A, position))
            self.focus_position = position
        except OSError as error:
            print(f"{self.device}: focus unavailable: {error}", flush=True)
            self.focus_steps = []
        self.focus_after = now + 0.3


def poll():
    active = readers()
    now = time.monotonic()
    for camera in cameras:
        if camera.device in active:
            camera.last_reader = now
            if not camera.running and now >= camera.retry_at:
                camera.start()
        elif camera.running and now - camera.last_reader > 5:
            camera.stop()
    return GLib.SOURCE_CONTINUE


def autofocus():
    now = time.monotonic()
    for camera in cameras:
        camera.focus(now)
    return GLib.SOURCE_CONTINUE


def main():
    global cameras
    deadline = time.monotonic() + 30
    while not all(os.access(path, os.R_OK | os.W_OK) for path in ("/dev/video42", "/dev/video43")):
        if time.monotonic() >= deadline:
            raise SystemExit("Camera loopback devices are unavailable")
        time.sleep(0.5)

    cameras = [Camera("/dev/video42", r"\_SB_.PC00.LNK0"),
               Camera("/dev/video43", r"\_SB_.PC00.LNK1")]
    notify_ready()
    loop = GLib.MainLoop()
    GLib.timeout_add_seconds(1, poll)
    GLib.timeout_add(100, autofocus)
    for sig in (signal.SIGINT, signal.SIGTERM):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, sig, loop.quit)
    try:
        loop.run()
    finally:
        for camera in cameras:
            camera.stop()
            os.close(camera.fd)


if __name__ == "__main__":
    main()
