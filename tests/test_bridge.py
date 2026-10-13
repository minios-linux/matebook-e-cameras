# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import os
from pathlib import Path
import socket
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("bridge", ROOT / "src/matebook_camera_bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class ReaderTests(unittest.TestCase):
    def test_only_other_processes_opening_public_devices_activate_cameras(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for pid, targets in {
                "100": ["/dev/video42", "/dev/video43"],
                "200": ["/dev/video42", "/dev/video8", "/dev/null"],
                "300": ["/dev/video43"],
            }.items():
                fds = root / pid / "fd"
                fds.mkdir(parents=True)
                for index, target in enumerate(targets):
                    (fds / str(index)).symlink_to(target)
            (root / "self").symlink_to("100")
            self.assertEqual(bridge.readers(root, own_pid=100),
                             {"/dev/video42", "/dev/video43"})
            self.assertEqual(bridge.readers(root, own_pid=200),
                             {"/dev/video42", "/dev/video43"})
            for descriptor in (root / "300/fd").iterdir():
                descriptor.unlink()
            self.assertEqual(bridge.readers(root, own_pid=100), {"/dev/video42"})


class FocusTests(unittest.TestCase):
    @staticmethod
    def image(luminance, chroma=lambda x, y: 128):
        return bytes(value for y in range(96) for x in range(96)
                     for value in (luminance(x, y), chroma(x, y)))

    def test_focus_uses_luminance_not_chroma(self):
        frame = self.image(lambda x, y: 80, lambda x, y: (x * 37 + y * 11) % 256)
        self.assertEqual(bridge.contrast_score(frame, 96, 96), 0)

    def test_focus_distinguishes_detail_and_normalizes_exposure(self):
        detail = lambda x, y: 40 if (x // 4) % 2 == 0 else 100
        flat = self.image(lambda x, y: 70)
        sharp = self.image(detail)
        brighter = self.image(lambda x, y: detail(x, y) * 2)
        self.assertGreater(bridge.contrast_score(sharp, 96, 96),
                           bridge.contrast_score(flat, 96, 96))
        self.assertAlmostEqual(bridge.contrast_score(sharp, 96, 96),
                               bridge.contrast_score(brighter, 96, 96))


class LifecycleTests(unittest.TestCase):
    def test_stop_retains_manager_and_restart_reuses_pipeline(self):
        camera = bridge.Camera.__new__(bridge.Camera)
        pipeline = mock.Mock()
        camera.pipeline = pipeline
        camera.running = True
        camera.lens_fd = None
        camera.focus_steps = []
        camera.fd = 99
        camera.blank = b"blank"
        camera.device = "/dev/video42"
        camera.camera_name = r"\_SB_.PC00.LNK0"
        with mock.patch.object(bridge.os, "write"), mock.patch.object(bridge.Gst, "parse_launch") as parse:
            camera.stop()
            self.assertIs(camera.pipeline, pipeline)
            self.assertFalse(camera.running)
            pipeline.set_state.assert_called_with(bridge.Gst.State.READY)
            camera.start()
            self.assertTrue(camera.running)
            pipeline.set_state.assert_called_with(bridge.Gst.State.PLAYING)
            parse.assert_not_called()

    def test_clients_start_one_sensor_and_idle_stops_only_that_sensor(self):
        front = mock.Mock(device="/dev/video42", running=False,
                          retry_at=0, last_reader=0)
        rear = mock.Mock(device="/dev/video43", running=True,
                         retry_at=0, last_reader=100)
        with mock.patch.object(bridge, "cameras", [front, rear], create=True), \
                mock.patch.object(bridge.time, "monotonic", return_value=103), \
                mock.patch.object(bridge, "readers", return_value={"/dev/video42"}):
            bridge.poll()
            front.start.assert_called_once()
            rear.stop.assert_not_called()
        front.running = True
        with mock.patch.object(bridge, "cameras", [front, rear], create=True), \
                mock.patch.object(bridge.time, "monotonic", return_value=110), \
                mock.patch.object(bridge, "readers", return_value={"/dev/video43"}):
            bridge.poll()
            front.stop.assert_called_once()
            rear.stop.assert_not_called()


class NotifyTests(unittest.TestCase):
    def test_ready_notification_reaches_systemd_socket(self):
        with tempfile.TemporaryDirectory() as directory:
            address = str(Path(directory) / "notify")
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as server:
                server.bind(address)
                server.settimeout(1)
                with mock.patch.dict(os.environ, {"NOTIFY_SOCKET": address}):
                    bridge.notify_ready()
                self.assertEqual(server.recv(128), b"READY=1")


if __name__ == "__main__":
    unittest.main()
