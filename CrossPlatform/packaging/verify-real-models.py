#!/usr/bin/env python3
"""Exercise the release binaries with the exact production model files."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys


def require_file(value: str, label: str) -> pathlib.Path:
    path = pathlib.Path(value).resolve()
    if not path.is_file():
        raise SystemExit(f"{label} is missing: {path}")
    return path


def runtime_environment(
    directory: pathlib.Path, toolchain_directory: pathlib.Path | None
) -> dict[str, str]:
    environment = os.environ.copy()
    variable = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
    inherited = environment.get(variable, "")
    directories = [value for value in (toolchain_directory, directory) if value]
    prefix = os.pathsep.join(str(value) for value in directories)
    environment[variable] = prefix + (os.pathsep + inherited if inherited else "")
    return environment


def run_asr(args: argparse.Namespace) -> None:
    result = subprocess.run(
        [str(args.asr_test), str(args.asr_model), str(args.wav)],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=180,
        env=runtime_environment(args.asr_runtime, args.toolchain_runtime),
    )
    if result.returncode != 0:
        raise SystemExit(
            "Production Parakeet fixture failed "
            f"(exit {result.returncode}).\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    transcript = [line for line in lines if not line.startswith("elapsed_ms=")]
    elapsed = [line for line in lines if line.startswith("elapsed_ms=")]
    if not transcript or not any(line.removeprefix("elapsed_ms=").isdigit() for line in elapsed):
        raise SystemExit("Production Parakeet fixture returned no transcript or timing result.")
    normalized_transcript = transcript[0].casefold()
    if not all(word in normalized_transcript for word in ("report", "friday")):
        raise SystemExit(
            "Production Parakeet fixture did not transcribe the expected key words: "
            f"{transcript[0]}"
        )
    print(f"Parakeet production fixture passed: {transcript[0][:160]}")


def run_polisher(args: argparse.Namespace) -> None:
    request_id = "release-fixture"
    request = {
        "id": request_id,
        "text": "so um i need to send it friday no wait thursday",
        "tone": "casual",
        "timeoutMs": 60000,
        "maxOutputTokens": 128,
    }
    payload = "\n".join(
        (json.dumps(request, separators=(",", ":")), '{"command":"quit"}', "")
    )
    environment = runtime_environment(args.polish_runtime, args.toolchain_runtime)
    llama_name = "llama.dll" if os.name == "nt" else "libllama.so"
    llama_library = args.polish_runtime / llama_name
    if not llama_library.is_file():
        raise SystemExit(f"production llama library is missing: {llama_library}")
    environment["LOCALFLOW_LLAMA_LIBRARY"] = str(llama_library)
    result = subprocess.run(
        [str(args.polish_worker), "--model", str(args.polish_model)],
        input=payload,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=180,
        env=environment,
    )
    if result.returncode != 0:
        raise SystemExit(
            "Production S1-mini fixture failed "
            f"(exit {result.returncode}).\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    messages: list[dict[str, object]] = []
    for line in result.stdout.splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            messages.append(value)
    if not messages or messages[0].get("ready") is not True:
        raise SystemExit("Production S1-mini worker did not report ready.")
    responses = [message for message in messages if message.get("id") == request_id]
    if len(responses) != 1 or responses[0].get("ok") is not True:
        detail = responses[0].get("error") if responses else "missing response"
        raise SystemExit(f"Production S1-mini request failed: {detail}")
    text = responses[0].get("text")
    if not isinstance(text, str) or not text.strip() or len(text) > 8000:
        raise SystemExit("Production S1-mini returned an invalid text payload.")
    if "<think>" in text.lower() or "</think>" in text.lower():
        raise SystemExit("Production S1-mini leaked hidden reasoning tags.")
    normalized_text = text.casefold()
    if (
        "thursday" not in normalized_text
        or "friday" in normalized_text
        or re.search(r"\bum\b", normalized_text)
    ):
        raise SystemExit(
            "Production S1-mini did not perform the expected correction and cleanup: "
            f"{text.strip()}"
        )
    print(f"S1-mini production fixture passed: {text.strip()[:160]}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--asr-test", required=True)
    parser.add_argument("--asr-runtime", required=True)
    parser.add_argument("--asr-model", required=True)
    parser.add_argument("--wav", required=True)
    parser.add_argument("--polish-worker", required=True)
    parser.add_argument("--polish-runtime", required=True)
    parser.add_argument("--polish-model", required=True)
    parser.add_argument("--toolchain-runtime")
    values = parser.parse_args()

    values.asr_test = require_file(values.asr_test, "ASR contract executable")
    values.asr_runtime = pathlib.Path(values.asr_runtime).resolve()
    values.asr_model = require_file(values.asr_model, "Parakeet model")
    values.wav = require_file(values.wav, "WAV fixture")
    values.polish_worker = require_file(values.polish_worker, "polish worker")
    values.polish_runtime = pathlib.Path(values.polish_runtime).resolve()
    values.polish_model = require_file(values.polish_model, "S1-mini model")
    values.toolchain_runtime = (
        pathlib.Path(values.toolchain_runtime).resolve()
        if values.toolchain_runtime
        else None
    )
    for directory, label in (
        (values.asr_runtime, "ASR runtime directory"),
        (values.polish_runtime, "polish runtime directory"),
    ):
        if not directory.is_dir():
            raise SystemExit(f"{label} is missing: {directory}")
    if values.toolchain_runtime and not values.toolchain_runtime.is_dir():
        raise SystemExit(
            f"toolchain runtime directory is missing: {values.toolchain_runtime}"
        )

    run_asr(values)
    run_polisher(values)
    return 0


if __name__ == "__main__":
    sys.exit(main())
