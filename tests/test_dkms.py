# SPDX-License-Identifier: GPL-2.0-only
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KERNEL = "6.12.111+deb13-amd64"


class DkmsInvocationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "source"
        self.headers = self.directory / "headers"
        (self.source / "drivers").mkdir(parents=True)
        self.headers.mkdir()
        shutil.copyfile(ROOT / "Makefile", self.source / "Makefile")
        shutil.copyfile(ROOT / "drivers/Makefile", self.source / "drivers/Makefile")
        self.trace = self.directory / "trace"
        # Stand in for the kernel build system, which loads M=/.../drivers.
        (self.headers / "Makefile").write_text(
            ".PHONY: modules clean\n"
            "include $(M)/Makefile\n"
            "modules clean:\n"
            '\t@printf "%s\\n" "$@" "$(M)" "$(KERNELRELEASE)" "$(obj-m)" > "$(TRACE_FILE)"\n'
        )

    def invoke(self, recipe, expected_goal):
        result = subprocess.run(
            ["bash", "-c",
             'kernel_source_dir="$1"; source "$2"; '
             f'command=${{{recipe}}}; '
             'read -r -a args <<< "$command"; '
             'exec "${args[@]}" -j2 KERNELRELEASE="$3" TRACE_FILE="$4"',
             "dkms-test", str(self.headers),
             str(ROOT / "debian/matebook-e-cameras-dkms.dkms"),
             KERNEL, str(self.trace)],
            cwd=self.source, env=dict(os.environ),
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        goal, module_dir, release, modules = self.trace.read_text().splitlines()
        self.assertEqual(goal, expected_goal)
        self.assertEqual(Path(module_dir), self.source / "drivers")
        self.assertEqual(release, KERNEL)
        self.assertEqual(set(modules.split()),
                         {"hi846.o", "s5k3l6-matebook.o", "ipu-bridge.o", "dw9714.o"})

    def test_dkms_make_with_injected_kernelrelease_reaches_kbuild(self):
        self.invoke("MAKE[0]", "modules")

    def test_dkms_clean_with_injected_kernelrelease_reaches_kbuild(self):
        self.invoke("CLEAN", "clean")


if __name__ == "__main__":
    unittest.main()
