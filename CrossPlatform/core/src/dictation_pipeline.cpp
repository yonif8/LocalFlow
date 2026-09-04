#include "localflow/core/dictation_pipeline.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <utility>

namespace localflow::core {
namespace {

using PipelineClock = std::chrono::steady_clock;

std::chrono::microseconds elapsed_since(PipelineClock::time_point start) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        PipelineClock::now() - start);
}

bool is_blank(const std::string& text) {
    return detail::trim_unicode_whitespace(text).empty();
}

bool cancellation_requested(const DictationRequest& request) noexcept {
    if (!request.is_cancelled) {
        return false;
    }
    try {
        return request.is_cancelled();
    } catch (...) {
        // Cancellation callbacks are platform-owned. If one becomes invalid,
        // fail closed before insertion instead of risking text in the wrong UI.
        return true;
    }
}

std::vector<std::string> protected_dictionary_terms(const ReplacementEngine& engine) {
    std::vector<std::string> result;
    result.reserve(engine.dictionary().rules.size());
    for (const auto& rule : engine.dictionary().rules) {
        result.push_back(rule.written);
    }
    return result;
}

bool final_text_contains(
    const std::string& final_text_key,
    const TerminologyMatch& match) {
    const auto canonical_key = ScreenTermExtractor::normalized(match.canonical);
    return !canonical_key.empty()
        && final_text_key.find(canonical_key) != std::string::npos;
}

bool is_learnable(const TerminologyMatch& match) {
    return match.source == TerminologySource::learned
        || (match.source == TerminologySource::screen
            && match.confidence >= 0.88
            && ScreenTermExtractor::is_persistent_candidate(match.canonical));
}

}  // namespace

DictationPipeline::DictationPipeline(
    ITranscriber& transcriber,
    ReplacementEngine replacement_engine,
    LearnedTerminologyBank& learned_terminology,
    ITextPolisher& polisher,
    ITextInserter& inserter,
    DictationPipelineConfiguration configuration)
    : transcriber_(transcriber),
      replacement_engine_(std::move(replacement_engine)),
      learned_terminology_(learned_terminology),
      polisher_(polisher),
      inserter_(inserter),
      configuration_(configuration) {}

DictationPipelineResult DictationPipeline::run(const DictationRequest& request) {
    const auto total_start = PipelineClock::now();
    DictationPipelineResult result;
    auto complete = [&](PipelineCompletion completion) {
        result.diagnostics.completion = completion;
        result.diagnostics.total_elapsed = elapsed_since(total_start);
        return std::move(result);
    };
    auto cancel_at = [&](CancellationBoundary boundary) {
        result.diagnostics.cancellation_boundary = boundary;
        return complete(PipelineCompletion::cancelled);
    };

    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::before_transcription);
    }

    const auto transcription_start = PipelineClock::now();
    try {
        result.output_text = transcriber_.transcribe(request.utterance);
        result.diagnostics.transcription.elapsed = elapsed_since(transcription_start);
    } catch (...) {
        result.diagnostics.transcription.elapsed = elapsed_since(transcription_start);
        result.diagnostics.transcription.outcome = PipelineStageOutcome::failed;
        result.output_text.clear();
        return complete(PipelineCompletion::transcription_failed);
    }
    if (is_blank(result.output_text)) {
        result.diagnostics.transcription.outcome = PipelineStageOutcome::empty;
        return complete(PipelineCompletion::empty_output);
    }
    result.diagnostics.transcription.outcome = PipelineStageOutcome::succeeded;
    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::after_transcription);
    }

    const auto replacements_start = PipelineClock::now();
    try {
        const auto replaced = replacement_engine_.apply(result.output_text);
        result.diagnostics.replacements_changed_text = replaced != result.output_text;
        result.output_text = replaced;
        result.diagnostics.replacements.elapsed = elapsed_since(replacements_start);
    } catch (...) {
        result.diagnostics.replacements.elapsed = elapsed_since(replacements_start);
        result.diagnostics.replacements.outcome = PipelineStageOutcome::failed;
        return complete(PipelineCompletion::processing_failed);
    }
    if (is_blank(result.output_text)) {
        result.diagnostics.replacements.outcome = PipelineStageOutcome::empty;
        return complete(PipelineCompletion::empty_output);
    }
    result.diagnostics.replacements.outcome = PipelineStageOutcome::succeeded;
    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::after_replacements);
    }

    const auto protected_terms = protected_dictionary_terms(replacement_engine_);
    std::vector<TerminologyMatch> pre_polish_matches;
    const auto terminology_start = PipelineClock::now();
    if (configuration_.screen_terminology_enabled) {
        try {
            auto screen_terms = request.press_context.screen_terms;
            if (request.press_context.screen_terms_if_ready) {
                try {
                    auto latest = request.press_context.screen_terms_if_ready();
                    screen_terms.insert(
                        screen_terms.end(),
                        std::make_move_iterator(latest.begin()),
                        std::make_move_iterator(latest.end()));
                } catch (...) {
                    // Screen context is opportunistic. Accessibility/OCR
                    // failure must never delay or discard the dictation.
                }
            }
            auto correction = TerminologyCorrector::correct(
                result.output_text,
                screen_terms,
                learned_terminology_.terms(),
                protected_terms);
            result.output_text = std::move(correction.text);
            pre_polish_matches = std::move(correction.matches);
            for (const auto& match : pre_polish_matches) {
                if (match.source == TerminologySource::screen) {
                    ++result.diagnostics.screen_match_count;
                } else {
                    ++result.diagnostics.learned_match_count;
                }
            }
            result.diagnostics.terminology.outcome = PipelineStageOutcome::succeeded;
        } catch (...) {
            result.diagnostics.terminology.elapsed = elapsed_since(terminology_start);
            result.diagnostics.terminology.outcome = PipelineStageOutcome::failed;
            return complete(PipelineCompletion::processing_failed);
        }
    } else {
        result.diagnostics.terminology.outcome = PipelineStageOutcome::skipped;
    }
    result.diagnostics.terminology.elapsed = elapsed_since(terminology_start);
    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::after_terminology);
    }

    const std::string deterministic_fallback = result.output_text;
    const auto polish_start = PipelineClock::now();
    try {
        auto polished = polisher_.polish(
            deterministic_fallback,
            PolishContext{request.press_context.target_app_id});
        polished = detail::trim_unicode_whitespace(polished);
        if (polished.empty()) {
            result.output_text = deterministic_fallback;
            result.diagnostics.polish.outcome = PipelineStageOutcome::failed_open;
        } else {
            result.diagnostics.polish_changed_text = polished != deterministic_fallback;
            result.output_text = std::move(polished);
            result.diagnostics.polish.outcome = PipelineStageOutcome::succeeded;
        }
    } catch (...) {
        result.output_text = deterministic_fallback;
        result.diagnostics.polish.outcome = PipelineStageOutcome::failed_open;
    }
    result.diagnostics.polish.elapsed = elapsed_since(polish_start);
    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::after_polish);
    }

    const auto post_terminology_start = PipelineClock::now();
    if (!pre_polish_matches.empty()) {
        try {
            std::vector<LearnedTerm> matched_terms;
            matched_terms.reserve(pre_polish_matches.size());
            for (const auto& match : pre_polish_matches) {
                LearnedTerm term;
                term.canonical = match.canonical;
                term.aliases = {match.heard};
                matched_terms.push_back(std::move(term));
            }
            auto final_correction = TerminologyCorrector::correct(
                result.output_text, {}, matched_terms, protected_terms);
            result.output_text = std::move(final_correction.text);
            result.diagnostics.post_polish_restoration_count
                = final_correction.matches.size();
            result.diagnostics.post_polish_terminology.outcome
                = PipelineStageOutcome::succeeded;
        } catch (...) {
            // The deterministic pre-polish result is safer than inserting a
            // polish result after its confirmed spellings could not be
            // reasserted.
            result.output_text = deterministic_fallback;
            result.diagnostics.polish.outcome = PipelineStageOutcome::failed_open;
            result.diagnostics.post_polish_terminology.outcome
                = PipelineStageOutcome::failed_open;
        }
    } else {
        result.diagnostics.post_polish_terminology.outcome
            = PipelineStageOutcome::skipped;
    }
    result.diagnostics.post_polish_terminology.elapsed
        = elapsed_since(post_terminology_start);
    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::after_post_polish_terminology);
    }

    if (is_blank(result.output_text)) {
        return complete(PipelineCompletion::empty_output);
    }

    std::vector<TerminologyMatch> accepted_matches;
    const auto final_text_key = ScreenTermExtractor::normalized(result.output_text);
    std::copy_if(
        pre_polish_matches.begin(), pre_polish_matches.end(),
        std::back_inserter(accepted_matches), [&](const auto& match) {
            return final_text_contains(final_text_key, match);
        });
    result.diagnostics.accepted_terminology_match_count = accepted_matches.size();

    // This final fence is intentionally adjacent to the only external side
    // effect. A stop request during restoration/match accounting must never
    // leak an old transcript into whichever app is focused now.
    if (cancellation_requested(request)) {
        return cancel_at(CancellationBoundary::before_insertion);
    }

    const auto insertion_start = PipelineClock::now();
    try {
        inserter_.insert(result.output_text);
        result.diagnostics.insertion.outcome = PipelineStageOutcome::succeeded;
    } catch (...) {
        result.diagnostics.insertion.elapsed = elapsed_since(insertion_start);
        result.diagnostics.insertion.outcome = PipelineStageOutcome::failed;
        return complete(PipelineCompletion::insertion_failed);
    }
    result.diagnostics.insertion.elapsed = elapsed_since(insertion_start);

    std::vector<TerminologyMatch> learnable_matches;
    std::copy_if(
        accepted_matches.begin(), accepted_matches.end(),
        std::back_inserter(learnable_matches), is_learnable);
    const auto learning_start = PipelineClock::now();
    if (learnable_matches.empty()) {
        result.diagnostics.learning.outcome = PipelineStageOutcome::skipped;
    } else {
        try {
            learned_terminology_.learn(
                learnable_matches, request.press_context.target_app_id);
            result.diagnostics.learning.outcome = PipelineStageOutcome::succeeded;
        } catch (...) {
            // Insertion already succeeded; a persistence-side memory failure
            // must not turn a successful dictation into a duplicate retry.
            result.diagnostics.learning.outcome = PipelineStageOutcome::failed;
        }
    }
    result.diagnostics.learning.elapsed = elapsed_since(learning_start);
    return complete(PipelineCompletion::inserted);
}

}  // namespace localflow::core
