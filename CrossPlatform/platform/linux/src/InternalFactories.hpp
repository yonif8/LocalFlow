#pragma once

#include "localflow/linux/Audio.hpp"
#include "localflow/linux/GlobalShortcut.hpp"
#include "localflow/linux/ScreenContext.hpp"
#include "localflow/linux/TextInsertion.hpp"

#include <memory>
#include <string>

namespace localflow::platform::linux::detail {

[[nodiscard]] std::unique_ptr<GlobalShortcutBackend> makeX11ShortcutBackend();
[[nodiscard]] std::unique_ptr<ScreenContextBackend> makeX11ScreenContextBackend();
[[nodiscard]] std::unique_ptr<AccessibilityTextInserter> makeAtSpiInserter();
[[nodiscard]] std::unique_ptr<Clipboard> makeCommandClipboard(
    SessionType session,
    std::string backend);
[[nodiscard]] std::unique_ptr<PasteInjector> makeX11PasteInjector();
[[nodiscard]] std::unique_ptr<AudioCaptureBackend> makeProcessAudioCapture(
    std::string backend);
[[nodiscard]] std::unique_ptr<AudioDucker> makeProcessAudioDucker(
    std::string backend);

}  // namespace localflow::platform::linux::detail
