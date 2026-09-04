#include "localflow/inference/S1MiniPolisher.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace localflow::inference;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    for (const Tone tone : {Tone::Casual, Tone::Neutral}) {
        PolishRequest request;
        request.transcript = "so um i need to send it friday no wait thursday";
        request.tone = tone;
        const auto prompt = S1MiniPolisher::promptFor(request);
        expect(prompt.find("You are a text normalizer for speech-to-text transcripts.") != std::string::npos,
               "prompt is missing the S1 system instruction");
        // Deliberately match macOS: S1's casual register weakens punctuation
        // cleanup, while semi-formal still preserves conversational wording.
        expect(prompt.find("[Styling: semi-formal] [Structure: prose] [Context: general]") != std::string::npos,
               "prompt is missing the reliable cleanup control line");
        expect(prompt.find("<think>\n\n</think>\n\n") != std::string::npos,
               "prompt did not disable Qwen thinking output");
        expect(prompt.find(request.transcript) != std::string::npos,
               "prompt lost the transcript");
    }

    S1MiniPolisher polisher({"/definitely/missing/s1.gguf", 4096, 0});
    const auto emptyResult = polisher.polish({});
    expect(bool(emptyResult), "empty polish input should succeed without loading a model");
    expect(emptyResult.value().text.empty(), "empty polish input should remain empty");

    std::cout << "polish contract tests passed\n";
    return 0;
}
