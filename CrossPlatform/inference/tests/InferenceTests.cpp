#include "localflow/inference/NemoTranscriber.hpp"
#include "localflow/inference/S1MiniPolisher.hpp"

#include <cassert>
#include <iostream>

using namespace localflow::inference;

int main() {
    PolishRequest request;
    request.transcript = "so um i need to send it friday no wait thursday";
    request.tone = Tone::Casual;
    const auto prompt = S1MiniPolisher::promptFor(request);
    assert(prompt.find("You are a text normalizer for speech-to-text transcripts.") != std::string::npos);
    assert(prompt.find("[Styling: semi-formal] [Structure: prose] [Context: general]") != std::string::npos);
    assert(prompt.find("<think>\n\n</think>\n\n") != std::string::npos);
    assert(prompt.find(request.transcript) != std::string::npos);

    NemoTranscriber transcriber({"/definitely/missing/parakeet.gguf", -1, ""});
    AudioBuffer shortAudio;
    shortAudio.samples.resize(12);
    const auto shortResult = transcriber.transcribe(shortAudio);
    assert(!shortResult);
    assert(shortResult.error().find("too short") != std::string::npos);

    S1MiniPolisher polisher({"/definitely/missing/s1.gguf", 4096, 0});
    const auto emptyResult = polisher.polish({});
    assert(emptyResult);
    assert(emptyResult.value().text.empty());

    std::cout << "inference contract tests passed\n";
    return 0;
}
