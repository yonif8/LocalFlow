#pragma once

#include "localflow/linux/Audio.hpp"
#include "localflow/linux/GlobalShortcut.hpp"
#include "localflow/linux/ScreenContext.hpp"
#include "localflow/linux/TextInsertion.hpp"

#include <memory>
#include <string>
#include <vector>

namespace localflow::platform::linux::detail {

[[nodiscard]] std::unique_ptr<GlobalShortcutBackend> makeX11ShortcutBackend();
[[nodiscard]] std::unique_ptr<ScreenContextBackend> makeX11ScreenContextBackend();
[[nodiscard]] std::unique_ptr<AccessibilityTextInserter> makeAtSpiInserter();
[[nodiscard]] std::shared_ptr<FocusedTargetProvider>
makeAtSpiFocusedTargetProvider();
[[nodiscard]] std::unique_ptr<Clipboard> makeCommandClipboard(
    SessionType session,
    std::string backend);
[[nodiscard]] std::unique_ptr<Clipboard> makeQtClipboard();
[[nodiscard]] std::unique_ptr<PasteInjector> makeX11PasteInjector();
[[nodiscard]] std::unique_ptr<AudioCaptureBackend> makeProcessAudioCapture(
    std::string backend);
[[nodiscard]] std::unique_ptr<AudioCaptureBackend> makeProcessAudioCaptureForTest(
    std::vector<std::string> command);
[[nodiscard]] std::unique_ptr<AudioDucker> makeProcessAudioDucker(
    std::string backend);
[[nodiscard]] std::shared_ptr<GlobalShortcutsPortal>
makeQDbusGlobalShortcutsPortal();
[[nodiscard]] std::shared_ptr<ScreenshotPortal>
makeQDbusScreenshotPortal();
[[nodiscard]] std::shared_ptr<RemoteDesktopPortal>
makeQDbusRemoteDesktopPortal();

}  // namespace localflow::platform::linux::detail
