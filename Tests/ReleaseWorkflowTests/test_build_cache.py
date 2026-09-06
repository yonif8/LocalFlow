"""Cache pruning must preserve dependencies and never follow external paths."""
import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("prune", ROOT / "Scripts/prune-macos-build-cache.py")
PRUNE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PRUNE)
PACKAGE = {"name": "LocalFlow", "targets": [
    {"name": "LocalFlowApp"}, {"name": "LFEngine"}, {"name": "LFEngineTests"},
    {"name": "engine-cli"}, {"name": "NewTarget"},
], "products": []}


class BuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="localflow-cache-tests-")
        self.addCleanup(self.temporary.cleanup)
        self.workspace = Path(self.temporary.name)
        (self.workspace / "Package.swift").touch()
        self.cache = self.workspace / ".build"
        self.configuration = self.cache / "arm64-apple-macosx/release"
        self.configuration.mkdir(parents=True)

    def file(self, relative):
        path = self.configuration / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture")
        return path

    def test_removes_app_tests_generated_modules_and_new_targets(self):
        paths = [self.file(name) for name in (
            "LocalFlowApp", "engine-cli", "engine_cli.build/main.o", "LFEngine.build/Engine.o",
            "LocalFlowPackageTests.xctest/Contents/MacOS/tests", "LFEngineTests.build/tests.o",
            "Modules/LFEngine.swiftmodule", "Modules/LocalFlowPackageTests.swiftdoc",
            "NewTarget.build/new.o", "Modules/NewTarget.swiftmodule",
            "LocalFlowApp.dSYM/Contents/Resources/DWARF/app", "LocalFlowPackageTests.derived/runner.swift",
            "Modules-tool/NewTarget.swiftmodule", "index/store/record",
        )]
        PRUNE.prune(self.workspace, PACKAGE)
        self.assertTrue(all(not path.exists() for path in paths))

    def test_preserves_dependency_outputs_build_database_and_downloads(self):
        paths = [self.file(name) for name in (
            "FluidAudio.build/Model.o", "Modules/FluidAudio.swiftmodule", "MLX.build/core.o",
            "description.json", "MLXHuggingFaceMacros-tool",
        )]
        for relative in ("build.db", "checkouts/FluidAudio/LFEngine.swift", "artifacts/Sparkle.zip"):
            path = self.cache / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("fixture")
            paths.append(path)
        PRUNE.prune(self.workspace, PACKAGE)
        self.assertTrue(all(path.exists() for path in paths))

    def test_product_symlink_is_unlinked_without_touching_destination(self):
        outside = self.workspace / "keep.txt"
        outside.write_text("keep")
        link = self.configuration / "LocalFlowApp"
        link.symlink_to(outside)
        PRUNE.prune(self.workspace, PACKAGE)
        self.assertFalse(link.is_symlink())
        self.assertEqual(outside.read_text(), "keep")

    def test_escaping_module_directory_is_rejected(self):
        outside = self.workspace / "outside"
        outside.mkdir()
        (outside / "LFEngine.swiftmodule").write_text("keep")
        (self.configuration / "Modules").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            PRUNE.prune(self.workspace, PACKAGE)
        self.assertEqual((outside / "LFEngine.swiftmodule").read_text(), "keep")

    def test_unsafe_target_name_is_rejected(self):
        with self.assertRaises(ValueError):
            PRUNE.prune(self.workspace, {"name": "LocalFlow", "targets": [{"name": "../outside"}]})

    def test_both_workflows_prune_before_post_job_cache_save(self):
        for name in ("macos", "release"):
            workflow = (ROOT / f".github/workflows/{name}.yml").read_text()
            self.assertIn("run: python3 Scripts/prune-macos-build-cache.py", workflow)
        setup = (ROOT / ".github/actions/setup-macos/action.yml").read_text()
        self.assertIn("python3 Scripts/prune-macos-build-cache.py", setup)


if __name__ == "__main__":
    unittest.main()
