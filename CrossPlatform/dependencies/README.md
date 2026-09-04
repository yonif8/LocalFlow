# Pinned native inference runtimes

These bootstraps install the exact CPU runtimes locked in
`runtime-lock.json`. There is no `latest` lookup, branch checkout, package
manager resolution, or source-build fallback. Every archive has both a fixed
URL and a hard-coded SHA-256; extraction begins only after the digest and byte
size match.

## Bootstrap

Linux prerequisites are Bash, `curl`, `tar`, and either `sha256sum` or
`shasum`:

```bash
CrossPlatform/dependencies/bootstrap-linux.sh
source CrossPlatform/dependencies/.runtime/linux-$(uname -m)/activate.sh
```

The script normalizes `amd64` to `x86_64` and `arm64` to `aarch64`. Override
the install parent without changing any version using `--prefix /absolute/path`.

On 64-bit Windows, use PowerShell 7 or Windows PowerShell 5.1 plus the
in-box `tar.exe` from Windows 10 1803 / Windows Server 2019 or newer:

```powershell
& .\CrossPlatform\dependencies\bootstrap-windows.ps1
$env:NEMO_SPEECH_ROOT
$env:LLAMA_ROOT
```

The PowerShell script sets both variables for its current process and writes
an `activate.ps1` for later shells. NeMo-Speech.cpp v0.1.0 did not publish a
Windows ARM64 CPU SDK, so ARM64 intentionally fails with an explanation. It
must not fall back to `main`, a different release, or an unpinned local build.

The stable roots are versioned and may be passed directly to CMake:

```text
NEMO_SPEECH_ROOT=.../<platform>-<arch>/nemo-speech-0.1.0-cpu
LLAMA_ROOT=.../<platform>-<arch>/llama-b10794-cpu
```

NeMo's root is its complete published SDK (headers, import/shared libraries,
notices, and CMake metadata). The llama root is normalized to `include`, `lib`
on Linux or `bin` on Windows, and `share/licenses`; its public `llama.h` and
ggml headers come from the exact release commit recorded in the lock.
The official b10794 Windows runtime does not publish a `llama.lib`; the polish
worker deliberately loads its private `llama.dll` C API at runtime instead of
linking it into the desktop process.

## Mandatory process isolation

The NeMo SDK contains ggml **0.12.0** and an unrelated bundled llama library.
The S1 runtime is llama.cpp b10794 with ggml **0.22.0**. Both use the same
loader names (`ggml`, `ggml-base`, and `llama`) despite incompatible ABIs.
They must never be copied into one directory, linked into one executable, or
added together to `PATH`/`LD_LIBRARY_PATH`.

LocalFlow therefore ships two loader namespaces:

- `LocalFlow` / the ASR process receives only the files listed under
  `runtimeIsolation.desktopProcess` in `runtime-lock.json`, copied from
  `NEMO_SPEECH_ROOT` into `bin/asr`. In particular, do **not** copy NeMo's
  llama, NMT, or TTS libraries.
- `localflow-polish-worker` is a persistent child process. Its private
  `bin/polish` directory receives only the llama/ggml CPU files listed under
  `runtimeIsolation.polishWorkerProcess`. On Windows it also receives the
  locked `libomp.dll`; on Linux OpenMP comes from the distribution runtime.
- The app and worker exchange bounded requests over IPC. The desktop process
  never loads a library from `LLAMA_ROOT`, and the worker never loads a
  library from `NEMO_SPEECH_ROOT`.

Those manifest arrays are the packaging allowlists. Do not replace them with
`*.dll`, `*.so*`, or a recursive copy: that would silently reintroduce the ABI
collision this layout exists to prevent. Preserve the complete license and
third-party-notice trees alongside distributed artifacts.

## Updating a runtime

Treat a runtime change like a source dependency update: choose an immutable
tag/commit, record every platform asset URL, size, SHA-256, architecture, and
license, update both scripts, inspect the extracted library closure, and run
ASR/polish tests in their separate packaged processes. A checksum failure is a
hard failure; never “fix” it by accepting the newly downloaded digest.
