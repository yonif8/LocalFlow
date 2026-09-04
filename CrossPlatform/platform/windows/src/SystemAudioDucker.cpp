#include "localflow/windows/SystemAudioDucker.hpp"

#ifdef _WIN32

#include "localflow/windows/AudioSafetyState.hpp"
#include "localflow/windows/WinError.hpp"

#include <Mmdeviceapi.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

namespace localflow::windows {
namespace {

using Microsoft::WRL::ComPtr;

// Allows volume-change listeners to identify LocalFlow-originated changes.
constexpr GUID kLocalFlowVolumeContext = {
    0x4cc5e20d, 0xe96d, 0x4d26, {0xb8, 0x76, 0x77, 0xb9, 0xdc, 0xe3, 0x96, 0x38}};

class ComApartment final {
public:
    ComApartment() {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(result_);
        if (result_ == RPC_E_CHANGED_MODE) {
            result_ = S_OK;
        }
    }
    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_{E_FAIL};
    bool initialized_{false};
};

std::wstring get_device_id(IMMDevice* device, HRESULT& result) {
    wchar_t* raw = nullptr;
    result = device->GetId(&raw);
    if (FAILED(result) || raw == nullptr) {
        return {};
    }
    std::wstring value(raw);
    CoTaskMemFree(raw);
    return value;
}

}  // namespace

SystemAudioDucker::~SystemAudioDucker() {
    (void)restore();
}

std::error_code SystemAudioDucker::duck(const float scale) {
    std::lock_guard lock(mutex_);
    if (active_) {
        return {};
    }
    if (!(scale >= 0.0F && scale <= 1.0F)) {
        return win32_error(ERROR_INVALID_PARAMETER);
    }

    ComApartment apartment;
    if (FAILED(apartment.result())) {
        return hresult_error(apartment.result());
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        return hresult_error(result);
    }
    ComPtr<IMMDevice> device;
    result = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    ComPtr<IAudioEndpointVolume> volume;
    result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    if (FAILED(result)) {
        return hresult_error(result);
    }

    float original = 1.0F;
    result = volume->GetMasterVolumeLevelScalar(&original);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    std::wstring id = get_device_id(device.Get(), result);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    const float requested_ducked = std::clamp(original * scale, 0.0F, 1.0F);
    result = volume->SetMasterVolumeLevelScalar(
        requested_ducked, &kLocalFlowVolumeContext);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    float applied_ducked = requested_ducked;
    // Endpoint curves can quantize scalar values. Record the round-tripped
    // value so the later compare-before-set does not mistake that for a user
    // adjustment.
    float observed_ducked = requested_ducked;
    if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&observed_ducked))) {
        applied_ducked = observed_ducked;
    }

    endpoint_id_ = std::move(id);
    original_volume_ = original;
    applied_ducked_volume_ = applied_ducked;
    active_ = true;
    return {};
}

std::error_code SystemAudioDucker::restore() noexcept {
    std::lock_guard lock(mutex_);
    if (!active_) {
        return {};
    }
    ComApartment apartment;
    if (FAILED(apartment.result())) {
        return hresult_error(apartment.result());
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        return hresult_error(result);
    }
    ComPtr<IMMDevice> device;
    result = enumerator->GetDevice(endpoint_id_.c_str(), &device);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    ComPtr<IAudioEndpointVolume> volume;
    result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    float current_volume = 0.0F;
    result = volume->GetMasterVolumeLevelScalar(&current_volume);
    if (FAILED(result)) {
        return hresult_error(result);
    }
    if (decide_volume_restore(active_, current_volume, applied_ducked_volume_)
        == VolumeRestoreDecision::restore_original) {
        result = volume->SetMasterVolumeLevelScalar(
            original_volume_, &kLocalFlowVolumeContext);
        if (FAILED(result)) {
            return hresult_error(result);
        }
    }

    // Mute is intentionally never read-modify-written. A mute/unmute choice
    // made before or during dictation remains exactly as the user left it.
    active_ = false;
    endpoint_id_.clear();
    original_volume_ = 1.0F;
    applied_ducked_volume_ = 1.0F;
    return {};
}

bool SystemAudioDucker::is_ducked() const noexcept {
    std::lock_guard lock(mutex_);
    return active_;
}

}  // namespace localflow::windows

#endif
