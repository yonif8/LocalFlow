#pragma once

// Convenience umbrella for the Windows application target. Individual headers
// remain usable independently so tests and small tools do not inherit every
// Win32 subsystem.

#include "localflow/windows/AudioCapture.hpp"
#include "localflow/windows/ClipboardTransaction.hpp"
#include "localflow/windows/ForegroundWindow.hpp"
#include "localflow/windows/InputMonitor.hpp"
#include "localflow/windows/PttStateMachine.hpp"
#include "localflow/windows/ScreenCapture.hpp"
#include "localflow/windows/SystemAudioDucker.hpp"
#include "localflow/windows/TextInserter.hpp"
#include "localflow/windows/WinError.hpp"
