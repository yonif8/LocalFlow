#include "localflow/inference/NemoTranscriber.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace localflow::inference;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t little16(const unsigned char* value) {
    return static_cast<std::uint16_t>(value[0]) |
        (static_cast<std::uint16_t>(value[1]) << 8U);
}

std::uint32_t little32(const unsigned char* value) {
    return static_cast<std::uint32_t>(value[0]) |
        (static_cast<std::uint32_t>(value[1]) << 8U) |
        (static_cast<std::uint32_t>(value[2]) << 16U) |
        (static_cast<std::uint32_t>(value[3]) << 24U);
}

AudioBuffer loadFixture(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    expect(bool(input), "could not open WAV fixture");
    unsigned char header[12]{};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    expect(input.gcount() == static_cast<std::streamsize>(sizeof(header)),
           "WAV header is truncated");
    expect(std::string(reinterpret_cast<char*>(header), 4) == "RIFF" &&
               std::string(reinterpret_cast<char*>(header + 8), 4) == "WAVE",
           "fixture is not a RIFF/WAVE file");

    std::uint16_t encoding = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t sampleRate = 0;
    std::vector<unsigned char> pcm;
    while (input && pcm.empty()) {
        unsigned char chunkHeader[8]{};
        input.read(reinterpret_cast<char*>(chunkHeader), sizeof(chunkHeader));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(chunkHeader))) break;
        const std::string id(reinterpret_cast<char*>(chunkHeader), 4);
        const auto size = little32(chunkHeader + 4);
        expect(size <= 100U * 1024U * 1024U, "WAV chunk is unreasonably large");
        std::vector<unsigned char> contents(size);
        input.read(reinterpret_cast<char*>(contents.data()),
                   static_cast<std::streamsize>(contents.size()));
        expect(input.gcount() == static_cast<std::streamsize>(contents.size()),
               "WAV chunk is truncated");
        if ((size & 1U) != 0U) input.get();
        if (id == "fmt ") {
            expect(contents.size() >= 16, "WAV format chunk is truncated");
            encoding = little16(contents.data());
            channels = little16(contents.data() + 2);
            sampleRate = little32(contents.data() + 4);
            bits = little16(contents.data() + 14);
        } else if (id == "data") {
            pcm = std::move(contents);
        }
    }
    expect(encoding == 1 && channels == 1 && bits == 16 && sampleRate == 16'000,
           "fixture must be mono 16 kHz PCM16");
    expect(!pcm.empty() && (pcm.size() % 2U) == 0U, "WAV contains no PCM samples");

    AudioBuffer result;
    result.sampleRate = static_cast<int>(sampleRate);
    result.samples.resize(pcm.size() / 2U);
    for (std::size_t index = 0; index < result.samples.size(); ++index) {
        const auto bitsValue = little16(pcm.data() + index * 2U);
        const auto value = static_cast<std::int16_t>(bitsValue);
        result.samples[index] = static_cast<float>(value) / 32768.0F;
    }
    return result;
}
}

int main(int argc, char** argv) {
    if (argc == 3) {
        NemoTranscriber transcriber({argv[1], -1, ""});
        const auto result = transcriber.transcribe(loadFixture(argv[2]));
        if (!result) {
            std::cerr << result.error() << '\n';
            return 2;
        }
        std::cout << result.value().text << '\n'
                  << "elapsed_ms=" << result.value().elapsed.count() << '\n';
        return result.value().text.empty() ? 3 : 0;
    }
    expect(argc == 1, "usage: localflow_asr_contract_tests [MODEL WAV]");
    NemoTranscriber transcriber({"/definitely/missing/parakeet.gguf", -1, ""});
    AudioBuffer shortAudio;
    shortAudio.samples.resize(12);
    const auto shortResult = transcriber.transcribe(shortAudio);
    expect(!shortResult, "audio below the minimum length should fail");
    expect(shortResult.error().find("too short") != std::string::npos,
           "short-audio failure should explain the problem");

    std::cout << "ASR contract tests passed\n";
    return 0;
}
