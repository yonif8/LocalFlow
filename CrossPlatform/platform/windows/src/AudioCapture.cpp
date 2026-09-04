#include "localflow/windows/AudioCapture.hpp"

#ifdef _WIN32

#include "localflow/windows/AudioSafetyState.hpp"
#include "localflow/windows/WinError.hpp"

#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace localflow::windows {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
public:
    ComApartment() {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(result_);
        if (result_ == RPC_E_CHANGED_MODE) {
            // The caller already owns a usable STA. Enumeration is apartment
            // neutral, so use it without balancing somebody else's init.
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

struct CoTaskMemWaveFormat {
    WAVEFORMATEX* value{nullptr};
    ~CoTaskMemWaveFormat() { CoTaskMemFree(value); }
};

std::wstring device_id(IMMDevice* device) {
    wchar_t* raw = nullptr;
    if (FAILED(device->GetId(&raw)) || raw == nullptr) {
        return {};
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::wstring device_name(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::wstring name;
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

enum class SampleEncoding {
    unsigned_pcm_8,
    signed_pcm_16,
    signed_pcm_24,
    signed_pcm_32,
    float_32,
    float_64,
    unsupported,
};

SampleEncoding encoding_for(const WAVEFORMATEX& format) noexcept {
    WORD tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        }
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (format.wBitsPerSample == 32) {
            return SampleEncoding::float_32;
        }
        if (format.wBitsPerSample == 64) {
            return SampleEncoding::float_64;
        }
    }
    if (tag == WAVE_FORMAT_PCM) {
        switch (format.wBitsPerSample) {
            case 8:
                return SampleEncoding::unsigned_pcm_8;
            case 16:
                return SampleEncoding::signed_pcm_16;
            case 24:
                return SampleEncoding::signed_pcm_24;
            case 32:
                return SampleEncoding::signed_pcm_32;
            default:
                break;
        }
    }
    return SampleEncoding::unsupported;
}

float read_sample(const BYTE* source, const SampleEncoding encoding) noexcept {
    switch (encoding) {
        case SampleEncoding::unsigned_pcm_8:
            return (static_cast<float>(*source) - 128.0F) / 128.0F;
        case SampleEncoding::signed_pcm_16: {
            std::int16_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return static_cast<float>(value) / 32768.0F;
        }
        case SampleEncoding::signed_pcm_24: {
            std::int32_t value = static_cast<std::int32_t>(source[0])
                | (static_cast<std::int32_t>(source[1]) << 8)
                | (static_cast<std::int32_t>(source[2]) << 16);
            if ((value & 0x00800000) != 0) {
                value |= static_cast<std::int32_t>(0xFF000000);
            }
            return static_cast<float>(value) / 8'388'608.0F;
        }
        case SampleEncoding::signed_pcm_32: {
            std::int32_t value = 0;
            std::memcpy(&value, source, sizeof(value));
            return static_cast<float>(static_cast<double>(value) / 2'147'483'648.0);
        }
        case SampleEncoding::float_32: {
            float value = 0;
            std::memcpy(&value, source, sizeof(value));
            return std::clamp(value, -1.0F, 1.0F);
        }
        case SampleEncoding::float_64: {
            double value = 0;
            std::memcpy(&value, source, sizeof(value));
            return static_cast<float>(std::clamp(value, -1.0, 1.0));
        }
        case SampleEncoding::unsupported:
            return 0;
    }
    return 0;
}

AudioChunk convert_packet(
    const BYTE* source,
    const UINT32 frame_count,
    const DWORD flags,
    const WAVEFORMATEX& format,
    const SampleEncoding encoding) {
    AudioChunk chunk;
    chunk.sample_rate_hz = format.nSamplesPerSec;
    chunk.captured_at = std::chrono::steady_clock::now();
    chunk.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
    chunk.samples.resize(frame_count, 0.0F);
    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || source == nullptr) {
        return chunk;
    }

    const std::size_t channels = format.nChannels;
    const std::size_t sample_bytes = format.wBitsPerSample / 8U;
    const std::size_t frame_bytes = format.nBlockAlign;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        float sum = 0.0F;
        const BYTE* frame_start = source + frame * frame_bytes;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            sum += read_sample(frame_start + channel * sample_bytes, encoding);
        }
        chunk.samples[frame] = sum / static_cast<float>(channels);
    }
    return chunk;
}

}  // namespace

std::vector<AudioDeviceInfo> enumerate_capture_devices(std::error_code& error) {
    error.clear();
    ComApartment apartment;
    if (FAILED(apartment.result())) {
        error = hresult_error(apartment.result());
        return {};
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        error = hresult_error(result);
        return {};
    }

    std::wstring default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(
            eCapture, eCommunications, &default_device))) {
        default_id = device_id(default_device.Get());
    }

    ComPtr<IMMDeviceCollection> collection;
    result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(result)) {
        error = hresult_error(result);
        return {};
    }
    UINT count = 0;
    result = collection->GetCount(&count);
    if (FAILED(result)) {
        error = hresult_error(result);
        return {};
    }

    std::vector<AudioDeviceInfo> devices;
    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) {
            continue;
        }
        AudioDeviceInfo info;
        info.id = device_id(device.Get());
        info.display_name = device_name(device.Get());
        info.is_default = !default_id.empty() && info.id == default_id;
        devices.push_back(std::move(info));
    }
    return devices;
}

WasapiMicrophoneCapture::WasapiMicrophoneCapture(
    AudioCaptureOptions options, ChunkCallback on_chunk, ErrorCallback on_error)
    : options_(std::move(options)),
      on_chunk_(std::move(on_chunk)),
      on_error_(std::move(on_error)) {}

WasapiMicrophoneCapture::~WasapiMicrophoneCapture() {
    stop();
}

std::error_code WasapiMicrophoneCapture::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (running_.load()) {
        return {};
    }
    if (capture_thread_.joinable()) {
        if (capture_thread_.get_id() == std::this_thread::get_id()) {
            return win32_error(ERROR_BUSY);
        }
        capture_thread_.join();
    }
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event_ == nullptr) {
        return last_win32_error();
    }

    std::promise<std::error_code> promise;
    auto future = promise.get_future();
    capture_thread_ = std::thread(&WasapiMicrophoneCapture::capture_main, this, std::move(promise));
    const auto error = future.get();
    if (error) {
        capture_thread_.join();
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
        return error;
    }
    return {};
}

void WasapiMicrophoneCapture::stop() noexcept {
    std::unique_lock lock(lifecycle_mutex_);
    if (stop_event_ != nullptr) {
        SetEvent(stop_event_);
    }
    lock.unlock();
    if (capture_thread_.joinable() && capture_thread_.get_id() != std::this_thread::get_id()) {
        capture_thread_.join();
    }
    lock.lock();
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
    running_.store(false);
}

void WasapiMicrophoneCapture::capture_main(std::promise<std::error_code> ready) noexcept {
    bool ready_sent = false;
    auto fail_start = [&](const HRESULT result) {
        if (!ready_sent) {
            ready.set_value(hresult_error(result));
            ready_sent = true;
        }
    };

    ComApartment apartment;
    if (FAILED(apartment.result())) {
        fail_start(apartment.result());
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        fail_start(result);
        return;
    }

    ComPtr<IMMDevice> device;
    const auto select_default_device = [&]() {
        device.Reset();
        HRESULT selection = enumerator->GetDefaultAudioEndpoint(
            eCapture, eCommunications, &device);
        if (selection == E_NOTFOUND) {
            selection = enumerator->GetDefaultAudioEndpoint(
                eCapture, eConsole, &device);
        }
        return selection;
    };
    bool using_configured_device = false;
    if (options_.endpoint_id.has_value()) {
        result = enumerator->GetDevice(options_.endpoint_id->c_str(), &device);
        DWORD state = 0;
        using_configured_device = SUCCEEDED(result)
            && SUCCEEDED(device->GetState(&state))
            && (state & DEVICE_STATE_ACTIVE) != 0;
        if (!using_configured_device) {
            result = select_default_device();
        }
    } else {
        result = select_default_device();
    }
    if (FAILED(result)) {
        fail_start(result);
        return;
    }

    ComPtr<IAudioClient> client;
    result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(result) && using_configured_device) {
        // An endpoint can disappear between GetState and activation. Retry once
        // with the current system default so unplugging a saved microphone does
        // not strand push-to-talk until the user revisits Settings.
        result = select_default_device();
        if (SUCCEEDED(result)) {
            client.Reset();
            result = device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        }
    }
    if (FAILED(result)) {
        fail_start(result);
        return;
    }

    CoTaskMemWaveFormat mix_format;
    result = client->GetMixFormat(&mix_format.value);
    if (FAILED(result)) {
        fail_start(result);
        return;
    }
    const auto encoding = encoding_for(*mix_format.value);
    if (encoding == SampleEncoding::unsupported || mix_format.value->nChannels == 0
        || mix_format.value->nBlockAlign == 0) {
        fail_start(AUDCLNT_E_UNSUPPORTED_FORMAT);
        return;
    }

    result = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        0,
        0,
        mix_format.value,
        nullptr);
    if (FAILED(result)) {
        fail_start(result);
        return;
    }

    HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (audio_event == nullptr) {
        if (!ready_sent) {
            ready.set_value(last_win32_error());
            ready_sent = true;
        }
        return;
    }
    result = client->SetEventHandle(audio_event);
    if (FAILED(result)) {
        CloseHandle(audio_event);
        fail_start(result);
        return;
    }

    ComPtr<IAudioCaptureClient> capture;
    result = client->GetService(IID_PPV_ARGS(&capture));
    if (FAILED(result)) {
        CloseHandle(audio_event);
        fail_start(result);
        return;
    }
    result = client->Start();
    if (FAILED(result)) {
        CloseHandle(audio_event);
        fail_start(result);
        return;
    }

    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
    running_.store(true);
    ready.set_value({});
    ready_sent = true;

    const HANDLE events[] = {stop_event_, audio_event};
    std::error_code runtime_error;
    auto consume_available_packets = [&]() noexcept {
        UINT32 packet_frames = 0;
        result = capture->GetNextPacketSize(&packet_frames);
        if (FAILED(result)) {
            runtime_error = hresult_error(result);
            return false;
        }
        while (packet_frames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            result = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) {
                runtime_error = hresult_error(result);
                return false;
            }
            AudioChunk chunk;
            try {
                chunk = convert_packet(
                    data, frames, flags, *mix_format.value, encoding);
            } catch (const std::bad_alloc&) {
                (void)capture->ReleaseBuffer(frames);
                runtime_error = win32_error(ERROR_NOT_ENOUGH_MEMORY);
                return false;
            } catch (...) {
                (void)capture->ReleaseBuffer(frames);
                runtime_error = win32_error(ERROR_UNHANDLED_EXCEPTION);
                return false;
            }
            result = capture->ReleaseBuffer(frames);
            if (FAILED(result)) {
                runtime_error = hresult_error(result);
                return false;
            }
            try {
                if (on_chunk_) {
                    on_chunk_(std::move(chunk));
                }
            } catch (...) {
                // Capture continuity is more important than a client callback.
            }
            result = capture->GetNextPacketSize(&packet_frames);
            if (FAILED(result)) {
                runtime_error = hresult_error(result);
                return false;
            }
        }
        return true;
    };

    bool stop_requested = false;
    std::chrono::steady_clock::time_point drain_deadline{};
    const auto drain_timeout = clamp_audio_tail_drain(options_.stop_drain_timeout);
    while (!runtime_error) {
        DWORD wait = WAIT_FAILED;
        if (!stop_requested) {
            wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        } else {
            const std::uint32_t remaining = audio_tail_wait_milliseconds(
                std::chrono::steady_clock::now(), drain_deadline);
            if (remaining == 0) {
                break;
            }
            const DWORD drain_wait = WaitForSingleObject(audio_event, remaining);
            if (drain_wait == WAIT_TIMEOUT) {
                break;
            }
            wait = drain_wait == WAIT_OBJECT_0 ? WAIT_OBJECT_0 + 1 : WAIT_FAILED;
        }

        if (wait == WAIT_OBJECT_0) {
            stop_requested = true;
            drain_deadline = std::chrono::steady_clock::now() + drain_timeout;
            if (!consume_available_packets()) {
                break;
            }
            continue;
        }
        if (wait == WAIT_OBJECT_0 + 1) {
            if (!consume_available_packets()) {
                break;
            }
            continue;
        }
        runtime_error = last_win32_error();
    }

    // A final non-blocking read catches a packet that became visible between
    // the last event and this stop boundary. stop() remains bounded because
    // this never waits. Do not query the capture client after Stop().
    if (!runtime_error) {
        (void)consume_available_packets();
    }
    result = client->Stop();
    if (FAILED(result) && !runtime_error) {
        runtime_error = hresult_error(result);
    }
    if (mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(mmcss);
    }
    CloseHandle(audio_event);
    running_.store(false);
    if (runtime_error && on_error_) {
        try {
            on_error_(runtime_error);
        } catch (...) {
        }
    }
}

}  // namespace localflow::windows

#endif
