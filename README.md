# LocalFlow

Fully-local dictation for macOS. Hold a key, speak, release — your words are
transcribed and typed into whatever app you're using. Nothing ever leaves your
Mac: no accounts, no cloud, no audio uploaded anywhere.

- **Hold-to-talk** — hold Fn (or a configurable key/mouse button), speak, release.
- **On-device speech recognition** — Granite ASR running on Apple Silicon via [MLX](https://github.com/ml-explore/mlx) ([Granite-MLX](https://github.com/kylehowells/Granite-MLX)).
- **On-device polish** — an optional fast LLM pass cleans up filler words and phrasing, powered by **S1-mini by Superwhisper**, also running locally via MLX.
- **Menu bar app** — history, settings, permissions, all in a lightweight status item.
- **Auto-updates** — via [Sparkle](https://sparkle-project.org); the app checks for updates and offers them in place.

Models (~1.2 GB) are downloaded automatically on first run.

Requires an Apple Silicon Mac running macOS 15 or later.

## Install

1. Download the latest `LocalFlow-<version>.dmg` from [Releases](https://github.com/yonif8/LocalFlow/releases).
2. Open the DMG and drag **LocalFlow** into **Applications**.
3. Double-click LocalFlow in Applications.

### First launch: "Apple could not verify…"

LocalFlow is a free, self-signed app (no Apple Developer subscription), so
Gatekeeper blocks the first launch. This is a **one-time** dance:

1. Double-click LocalFlow. A dialog says *"Apple could not verify 'LocalFlow'
   is free of malware…"* — click **Done** (**not** "Move to Trash").
2. Open **System Settings → Privacy & Security**, scroll down to the
   **Security** section. You'll see *"LocalFlow was blocked to protect your
   Mac."* — click **Open Anyway**.
3. Confirm in the dialog that appears and authenticate (password / Touch ID).

LocalFlow now opens normally forever after — including after auto-updates.

> **macOS 26 note:** the **Open Anyway** button only appears for about an hour
> after the blocked launch attempt. If you don't see it, double-click LocalFlow
> again and re-check System Settings.

<details>
<summary>Terminal alternative (skips the dialogs)</summary>

```sh
xattr -dr com.apple.quarantine /Applications/LocalFlow.app
```

</details>

### First run

LocalFlow lives in the menu bar (waveform icon). On first run it opens a
Permissions window — grant it:

- **Microphone** — to hear you,
- **Input Monitoring** — to detect the hold-to-talk key,
- **Accessibility** — to type the transcript into the frontmost app.

Then it downloads the speech models (~1.2 GB, one time) and you're set: hold
**Fn**, talk, release.

## Updates

LocalFlow checks for updates automatically (Sparkle, EdDSA-signed). You can
also check manually via the menu bar icon → **Check for Updates…**.

## Building from source

```sh
Scripts/setup-signing.sh   # once: stable self-signed identity (TCC persistence)
Scripts/make-app.sh        # dev build -> dist/LocalFlow.app
Scripts/release.sh 1.2.3   # versioned build + DMG + signed appcast
```

## Credits

- Speech recognition: IBM **Granite** ASR, via [Granite-MLX](https://github.com/kylehowells/Granite-MLX)
- Polish LLM: **S1-mini by Superwhisper**
- Runtime: [MLX / mlx-swift](https://github.com/ml-explore/mlx-swift), [mlx-swift-lm](https://github.com/ml-explore/mlx-swift-lm)
- Updates: [Sparkle](https://sparkle-project.org)
