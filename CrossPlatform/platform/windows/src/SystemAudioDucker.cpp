#include "localflow/windows/SystemAudioDucker.hpp"

#ifdef _WIN32

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
    result = volume->SetMasterVolumeLevelScalar(
        std::clamp(original * scale, 0.0F, 1.0F), &kLocalFlowVolumeContext);
    if (FAILED(result)) {
        return hresult_error(result);
    }

    endpoint_id_ = std::move(id);
    original_volume_ = original;
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
    result = volume->SetMasterVolumeLevelScalar(original_volume_, &kLocalFlowVolumeContext);
    if (FAILED(result)) {
        return hresult_error(result);
    }

    active_ = false;
    endpoint_id_.clear();
    return {};
}

bool SystemAudioDucker::is_ducked() const noexcept {
    std::lock_guard lock(mutex_);
    return active_;
}

}  // namespace localflow::windows

#endif
