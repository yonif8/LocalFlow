#!/usr/bin/env python3
"""Keep compiled dependencies, never cache LocalFlow app/test build products.

Called only after a successful CI build/test/verification. Derive root target
names from SwiftPM instead of maintaining a list that can miss a new target.
Never walk dependency checkouts, downloads, user directories, or symlinks.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess


def root_product_names(package):
    package_name = package["name"]
    names = {package_name + "PackageTests", package_name + "PackageDiscoveredTests"}
    names.update(target["name"] for target in package["targets"])
    names.update(product["name"] for product in package.get("products", []))
    if any(not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", name) for name in names):
        raise ValueError("Unsupported root target name; do not save compiled cache")
    names.update(name.replace("-", "_") for name in tuple(names))
    names.update(target["name"] + "-tool" for target in package["targets"]
                 if target.get("type") == "macro")
    names.update(package_name + "_" + target["name"] for target in package["targets"])
    return names


def prune(workspace, package, cache_name=".build"):
    workspace = Path(workspace).resolve()
    if cache_name not in (".build", ".build-release"):
        raise ValueError("Only isolated SwiftPM scratch directories are supported")
    cache = workspace / cache_name
    if not (workspace / "Package.swift").is_file():
        raise ValueError("Expected a Swift package workspace")
    if cache.is_symlink() or not cache.is_dir():
        raise ValueError("Expected a real workspace-owned .build directory")
    names = root_product_names(package)
    products = {name + suffix for name in names
                for suffix in ("", ".build", ".product", ".xctest", ".bundle", ".resources", ".dSYM", ".derived")}
    products.update("lib" + name + suffix for name in names for suffix in (".a", ".dylib"))
    removed = []

    def remove(path):
        # Unlink symlinks without following them. Ordinary paths must resolve
        # inside the exact cache tree, never through an escaping parent link.
        if not path.parent.resolve().is_relative_to(cache):
            raise ValueError("Refusing an escaping cache path")
        if path.is_symlink() or path.is_file():
            path.unlink()
        else:
            shutil.rmtree(path)
        removed.append(str(path.relative_to(cache)))

    for configuration in cache.glob("*-apple-macosx*/release"):
        if configuration.is_symlink() or not configuration.resolve().is_relative_to(cache):
            raise ValueError("Refusing an escaping build configuration")
        for entry in configuration.iterdir():
            if entry.name in products:
                remove(entry)
        # Index records are not needed for CI and can retain root source data.
        index = configuration / "index"
        if index.exists() or index.is_symlink():
            remove(index)
        for modules in (configuration / "Modules", configuration / "Modules-tool"):
            if not modules.exists():
                continue
            if modules.is_symlink() or not modules.resolve().is_relative_to(cache):
                raise ValueError("Refusing an escaping module directory")
            for entry in modules.iterdir():
                if any(entry.name == name or entry.name.startswith(name + ".") for name in names):
                    remove(entry)
    return removed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch-path", choices=(".build", ".build-release"), default=".build")
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    metadata = json.loads(subprocess.check_output(
        ["swift", "package", "dump-package"], cwd=root, text=True))
    removed = prune(root, metadata, arguments.scratch_path)
    print(f"Excluded {len(removed)} root app/test build products from the dependency cache.")
