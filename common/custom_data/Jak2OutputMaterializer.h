#pragma once

#include "common/custom_data/Jak1OutputMaterializer.h"
#include "common/custom_data/Jak2OutputRecipe.h"

namespace jak2_output_materializer {

inline constexpr std::size_t kNtscV2CompressedArchiveAlignmentBytes = 0x40000;

using GeneratedObjectArtifact = jak1_output_materializer::GeneratedObjectArtifact;
using GeneratedFlatArtifact = jak1_output_materializer::GeneratedFlatArtifact;
using Inputs = jak1_output_materializer::Inputs;
using Phase = jak1_output_materializer::Phase;
using Progress = jak1_output_materializer::Progress;
using CancelCallback = jak1_output_materializer::CancelCallback;
using ProgressCallback = jak1_output_materializer::ProgressCallback;
using Limits = jak1_output_materializer::Limits;
using ErrorCode = jak1_output_materializer::ErrorCode;
using Error = jak1_output_materializer::Error;
using Summary = jak1_output_materializer::Summary;

template <typename T>
using Result = jak1_output_materializer::Result<T>;

struct Options {
  Limits limits;
  jak2_output_recipe::Limits recipe_limits;
  CancelCallback should_cancel;
  ProgressCallback on_progress;

  Options();
};

/// Materialize a recipe after the caller has validated the disc revision and recorded source pack.
Result<Summary> materialize(const Inputs& inputs,
                            const std::filesystem::path& destination_root,
                            const jak2_iso::Revision& revision,
                            const Options& options = {});

inline const char* error_code_name(ErrorCode code) {
  return jak1_output_materializer::error_code_name(code);
}

}  // namespace jak2_output_materializer
