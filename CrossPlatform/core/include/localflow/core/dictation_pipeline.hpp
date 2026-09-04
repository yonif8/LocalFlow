#pragma once

#include "localflow/core/contracts.hpp"
#include "localflow/core/learned_terminology.hpp"
#include "localflow/core/replacements.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace localflow::core {

struct PressTimeContext {
    std::vector<std::string> screen_terms;
    std::optional<std::string> target_app_id;

    // Called after ASR, immediately before terminology correction, so press-
    // time OCR can run concurrently with recording and transcription. It must
    // never wait: return the latest completed snapshot or an empty vector.
    std::function<std::vector<std::string>()> screen_terms_if_ready;
};

struct DictationRequest {
    Utterance utterance;
    PressTimeContext press_context;

    // Optional cooperative cancellation check. It is evaluated only at safe
    // stage boundaries; no learned state is committed before insertion.
    std::function<bool()> is_cancelled;
};

struct DictationPipelineConfiguration {
    bool screen_terminology_enabled{true};
};

enum class PipelineCompletion {
    inserted,
    empty_output,
    cancelled,
    transcription_failed,
    processing_failed,
    insertion_failed,
};

enum class PipelineStageOutcome {
    not_run,
    succeeded,
    skipped,
    empty,
    failed,
    failed_open,
};

enum class CancellationBoundary {
    none,
    before_transcription,
    after_transcription,
    after_replacements,
    after_terminology,
    after_polish,
    after_post_polish_terminology,
};

struct PipelineStageDiagnostic {
    PipelineStageOutcome outcome{PipelineStageOutcome::not_run};
    std::chrono::microseconds elapsed{0};
};

// Deliberately content-free: safe to write to logs or attach to a bug report.
struct DictationPipelineDiagnostics {
    PipelineCompletion completion{PipelineCompletion::processing_failed};
    CancellationBoundary cancellation_boundary{CancellationBoundary::none};
    PipelineStageDiagnostic transcription;
    PipelineStageDiagnostic replacements;
    PipelineStageDiagnostic terminology;
    PipelineStageDiagnostic polish;
    PipelineStageDiagnostic post_polish_terminology;
    PipelineStageDiagnostic insertion;
    PipelineStageDiagnostic learning;
    std::chrono::microseconds total_elapsed{0};
    std::size_t screen_match_count{0};
    std::size_t learned_match_count{0};
    std::size_t post_polish_restoration_count{0};
    std::size_t accepted_terminology_match_count{0};
    bool replacements_changed_text{false};
    bool polish_changed_text{false};
};

struct DictationPipelineResult {
    // Kept separate from diagnostics so callers cannot accidentally include
    // dictated text when serializing the diagnostic object.
    std::string output_text;
    DictationPipelineDiagnostics diagnostics;

    [[nodiscard]] bool inserted() const noexcept {
        return diagnostics.completion == PipelineCompletion::inserted;
    }
};

// Synchronous by design: platform applications choose their own executor and
// cancellation/lifetime model. One instance must not be run concurrently.
class DictationPipeline {
public:
    DictationPipeline(
        ITranscriber& transcriber,
        ReplacementEngine replacement_engine,
        LearnedTerminologyBank& learned_terminology,
        ITextPolisher& polisher,
        ITextInserter& inserter,
        DictationPipelineConfiguration configuration = {});

    [[nodiscard]] DictationPipelineResult run(const DictationRequest& request);

private:
    ITranscriber& transcriber_;
    ReplacementEngine replacement_engine_;
    LearnedTerminologyBank& learned_terminology_;
    ITextPolisher& polisher_;
    ITextInserter& inserter_;
    DictationPipelineConfiguration configuration_;
};

}  // namespace localflow::core
