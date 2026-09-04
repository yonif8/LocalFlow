#!/usr/bin/env python3
"""Stage only the locked LocalFlow runtime allowlists into an app image."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path
from typing import NoReturn


def fail(message: str) -> NoReturn:
    raise SystemExit(f"localflow runtime staging: {message}")


def copy_allowlist(source: Path, destination: Path, names: list[str]) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for name in names:
        if Path(name).name != name:
            fail(f"manifest entry is not a basename: {name}")
        item = source / name
        if not item.exists() and not item.is_symlink():
            fail(f"locked runtime file is missing: {item}")
        target = destination / name
        if target.exists() or target.is_symlink():
            if target.is_dir() and not target.is_symlink():
                fail(f"refusing to replace a directory: {target}")
            target.unlink()
        if item.is_symlink():
            os.symlink(os.readlink(item), target)
        else:
            shutil.copy2(item, target)


def copy_licenses(root: Path, stage: Path, platform: str, component: str) -> None:
    source = root / "share" / "licenses" / component
    if not source.is_dir():
        fail(f"license directory is missing: {source}")
    prefix = stage / ("share" if platform == "windows" else "usr/share") / "licenses"
    shutil.copytree(source, prefix / component, dirs_exist_ok=True)


def select_component(manifest: dict[str, object], component_id: str) -> dict[str, object]:
    matches = [
        component
        for component in manifest.get("components", [])
        if isinstance(component, dict) and component.get("id") == component_id
    ]
    if len(matches) != 1:
        fail(f"runtime lock must contain exactly one {component_id} component")
    return matches[0]


def select_asset(
    component: dict[str, object], platform: str, architecture: str
) -> dict[str, object]:
    matches = [
        asset
        for asset in component.get("assets", [])
        if isinstance(asset, dict)
        and asset.get("platform") == platform
        and asset.get("architecture") == architecture
    ]
    if len(matches) != 1:
        fail(
            "runtime lock must contain exactly one "
            f"{component.get('id')} {platform}-{architecture} asset"
        )
    return matches[0]


def require_lock_string(mapping: dict[str, object], key: str, label: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value or "\n" in value or "\r" in value:
        fail(f"runtime lock has an invalid {label} {key}")
    return value


def expected_marker(
    component: dict[str, object],
    asset: dict[str, object],
    platform: str,
    architecture: str,
) -> str:
    component_id = require_lock_string(component, "id", "component")
    version = require_lock_string(component, "version", component_id)
    archive_sha256 = require_lock_string(asset, "sha256", component_id)
    if len(archive_sha256) != 64 or any(
        character not in "0123456789abcdef" for character in archive_sha256
    ):
        fail(f"runtime lock has an invalid {component_id} asset SHA-256")
    lines = [
        f"component={component_id}",
        f"version={version}",
    ]
    if component_id == "llama-cpp":
        commit = require_lock_string(component, "commit", component_id)
        source = component.get("sourceHeaders")
        if not isinstance(source, dict):
            fail("runtime lock is missing llama-cpp sourceHeaders")
        source_sha256 = require_lock_string(source, "sha256", component_id)
        if len(source_sha256) != 64 or any(
            character not in "0123456789abcdef" for character in source_sha256
        ):
            fail("runtime lock has an invalid llama-cpp source SHA-256")
        lines.append(f"commit={commit}")
    lines.extend(
        (
            f"platform={platform}",
            f"architecture={architecture}",
        )
    )
    if component_id == "nemo-speech-cpp":
        lines.append(f"archive_sha256={archive_sha256}")
    elif component_id == "llama-cpp":
        lines.extend(
            (
                f"runtime_sha256={archive_sha256}",
                f"source_sha256={source_sha256}",
            )
        )
    else:
        fail(f"unsupported runtime component in lock: {component_id}")
    return "\n".join(lines)


def verify_marker(root: Path, component: str, expected: str) -> None:
    marker = root / ".localflow-runtime-lock"
    if not marker.is_file() or marker.is_symlink():
        fail(f"{component} root has no regular bootstrap marker: {root}")
    try:
        actual = marker.read_text(encoding="ascii")
    except (OSError, UnicodeError) as error:
        fail(f"cannot read {component} bootstrap marker: {error}")
    if actual != expected:
        fail(
            f"{component} root marker does not match the current runtime lock: {root}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=("windows", "linux"), required=True)
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--nemo-root", type=Path, required=True)
    parser.add_argument("--llama-root", type=Path, required=True)
    args = parser.parse_args()

    stage = args.stage.resolve()
    if not stage.is_dir() or stage == Path(stage.anchor):
        fail("--stage must be an existing, non-root directory")
    manifest_path = Path(__file__).with_name("runtime-lock.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schemaVersion") != 1:
        fail("unsupported runtime lock schema")
    nemo_component = select_component(manifest, "nemo-speech-cpp")
    llama_component = select_component(manifest, "llama-cpp")
    nemo_asset = select_asset(nemo_component, args.platform, args.arch)
    llama_asset = select_asset(llama_component, args.platform, args.arch)

    nemo = args.nemo_root.resolve()
    llama = args.llama_root.resolve()
    verify_marker(
        nemo,
        "NeMo",
        expected_marker(
            nemo_component, nemo_asset, args.platform, args.arch
        ) + ("\n" if args.platform == "linux" else ""),
    )
    verify_marker(
        llama,
        "llama",
        expected_marker(
            llama_component, llama_asset, args.platform, args.arch
        ) + ("\n" if args.platform == "linux" else ""),
    )

    isolation = manifest["runtimeIsolation"]
    desktop = isolation["desktopProcess"]
    polish = isolation["polishWorkerProcess"]

    if args.platform == "windows":
        if args.arch != "x86_64":
            fail("Windows ARM64 is unsupported by the locked NeMo runtime")
        nemo_source = nemo / "bin"
        llama_source = llama / "bin"
        nemo_destination = stage / desktop["windowsDestination"]
        llama_destination = stage / polish["windowsDestination"]
        nemo_files = desktop["windowsFiles"]
        llama_files = polish["windowsCommonFiles"] + polish["windowsX86_64CpuFiles"]
    else:
        nemo_source = nemo / "lib"
        llama_source = llama / "lib"
        nemo_destination = stage / desktop["linuxDestination"]
        llama_destination = stage / polish["linuxDestination"]
        nemo_files = desktop["linuxFiles"]
        cpu_key = "linuxX86_64CpuFiles" if args.arch == "x86_64" else "linuxAarch64CpuFiles"
        llama_files = polish["linuxCommonFiles"] + polish[cpu_key]

    copy_allowlist(nemo_source, nemo_destination, nemo_files)
    copy_allowlist(llama_source, llama_destination, llama_files)
    copy_licenses(nemo, stage, args.platform, "nemo-speech")
    copy_licenses(llama, stage, args.platform, "llama.cpp")

    overlap = set(nemo_files) & set(llama_files)
    if overlap and nemo_destination == llama_destination:
        fail("incompatible runtime files share a destination: " + ", ".join(sorted(overlap)))
    print(f"NEMO_RUNTIME_DIR={nemo_destination}")
    print(f"LLAMA_RUNTIME_DIR={llama_destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
