"""Fast release-orchestration tests; no models, signing keys, or app launches."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "Scripts/lib/macos-build.sh"


class MacOSBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="localflow-build-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.scratch = Path(self.temporary.name) / "build with spaces"
        self.developer = Path(self.temporary.name) / "Xcode.app/Contents/Developer"
        self.developer.mkdir(parents=True)

    def run_shell(self, body, **extra_env):
        env = dict(os.environ, DEVELOPER_DIR=str(self.developer),
                   TEST_SCRATCH=str(self.scratch), GITHUB_STEP_SUMMARY="")
        env.update(extra_env)
        return subprocess.run(
            ["bash", "-c", 'set -euo pipefail; source "$1"; ' + body, "test", str(HELPER)],
            text=True, capture_output=True, env=env, cwd=ROOT,
        )

    def build_script(self, produce_app=True):
        create = 'mkdir -p "$TEST_SCRATCH/release"; touch "$TEST_SCRATCH/release/LocalFlowApp"; chmod +x "$TEST_SCRATCH/release/LocalFlowApp"' if produce_app else ":"
        return '''
            git() { echo "GIT:$*"; [[ "${FAIL_STAGE:-}" != lock ]]; }
            swift() {
                echo "SWIFT:$*"
                [[ "$1" != "${FAIL_STAGE:-}" ]] || return 17
                if [[ "$1" == test ]]; then CREATE_APP; fi
            }
            lf_build_and_test_macos "$TEST_SCRATCH"
        '''.replace("CREATE_APP", create)

    def test_one_test_command_builds_app_and_runs_tests(self):
        result = self.run_shell(self.build_script())
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = [line for line in result.stdout.splitlines() if line.startswith("SWIFT:")]
        self.assertEqual(len(calls), 2, calls)
        self.assertTrue(calls[0].startswith("SWIFT:package "))
        self.assertTrue(calls[1].startswith("SWIFT:test "))
        self.assertNotIn("--product", calls[1])
        self.assertNotIn("--skip-build", calls[1])
        self.assertIn("--parallel", calls[1])
        self.assertIn("--disable-automatic-resolution", calls[1])
        self.assertIn("--disable-build-manifest-caching", calls[1])
        self.assertEqual(result.stdout.count("TIMING:"), 2)

    def test_resolution_failure_stops_before_build(self):
        result = self.run_shell(self.build_script(), FAIL_STAGE="package")
        self.assertEqual(result.returncode, 17)
        self.assertNotIn("SWIFT:test", result.stdout)

    def test_changed_lock_stops_before_build(self):
        result = self.run_shell(self.build_script(), FAIL_STAGE="lock")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("SWIFT:test", result.stdout)

    def test_build_or_test_failure_never_reuses_old_app(self):
        self.scratch.joinpath("release").mkdir(parents=True)
        self.scratch.joinpath("release/LocalFlowApp").touch(mode=0o755)
        result = self.run_shell(self.build_script(), FAIL_STAGE="test")
        self.assertEqual(result.returncode, 17)

    def test_missing_app_is_not_a_successful_build(self):
        result = self.run_shell(self.build_script(produce_app=False))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not produce LocalFlowApp", result.stderr)

    def test_test_failure_is_propagated(self):
        result = self.run_shell(self.build_script(), FAIL_STAGE="test")
        self.assertEqual(result.returncode, 17)

    def preflight(self, version="6.2.3", **extra_env):
        return self.run_shell('''
            uname() { if [[ "$1" == -s ]]; then echo Darwin; else echo arm64; fi; }
            xcodebuild() { echo "Xcode fixture"; }
            swift() { echo "Apple Swift version $TEST_SWIFT_VERSION"; }
            xcrun() { [[ "${FAIL_METAL:-}" != true ]]; }
            lf_macos_preflight local "${CHECK_METAL:-false}"
        ''', TEST_SWIFT_VERSION=version, **extra_env)

    def test_old_swift_is_rejected_early(self):
        result = self.preflight("6.1.2")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires Swift 6.2", result.stderr)

    def test_supported_and_newer_swift_are_accepted(self):
        for version in ("6.2.0", "6.3.3", "7.0.0"):
            with self.subTest(version=version):
                result = self.preflight(version)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_unknown_swift_version_is_rejected(self):
        self.assertNotEqual(self.preflight("unavailable").returncode, 0)

    def test_missing_full_xcode_is_rejected(self):
        result = self.preflight(DEVELOPER_DIR=str(self.developer / "missing"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("full Xcode", result.stderr)

    def test_missing_metal_toolchain_is_rejected_before_packaging(self):
        result = self.preflight(CHECK_METAL="true", FAIL_METAL="true")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Metal Toolchain", result.stderr)

    def test_normal_ci_and_release_use_shared_setup_and_build(self):
        for name in ("macos", "release"):
            workflow = (ROOT / f".github/workflows/{name}.yml").read_text()
            self.assertIn("uses: ./.github/actions/setup-macos", workflow)
            self.assertIn("bash Scripts/test-macos.sh", workflow)
            self.assertNotIn("Xcode_26.", workflow)
            self.assertNotIn("swift build --configuration release --product", workflow)

    def test_packager_owns_build_and_test_without_unverified_skip_flag(self):
        release = (ROOT / "Scripts/release.sh").read_text()
        packager = (ROOT / "Scripts/make-app.sh").read_text()
        self.assertIn('--version "$VERSION" --test', release)
        self.assertNotIn("swift test", release)
        self.assertIn('lf_build_and_test_macos "$SCRATCH_DIR"', packager)
        self.assertNotIn("--skip-build", packager)
        self.assertLess(packager.index('lf_build_and_test_macos "$SCRATCH_DIR"'),
                        packager.index('rm -rf "$APP"'))


if __name__ == "__main__":
    unittest.main()
