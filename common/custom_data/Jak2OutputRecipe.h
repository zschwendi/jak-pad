#pragma once

#include <span>
#include <vector>

#include "common/custom_data/Jak1OutputRecipe.h"
#include "common/custom_data/Jak2SourceObjectPack.h"
#include "common/versions/jak2_iso_revisions.h"

namespace jak2_output_recipe {

using OutputProfile = jak1_output_recipe::OutputProfile;
using RevisionProvenance = jak1_output_recipe::RevisionProvenance;
using SourceObjectPackIdentity = jak1_output_recipe::SourceObjectPackIdentity;
using BundledSourceObject = jak1_output_recipe::BundledSourceObject;
using VerifiedRetailObject = jak1_output_recipe::VerifiedRetailObject;
using GeneratedDataKind = jak1_output_recipe::GeneratedDataKind;
using GeneratedData = jak1_output_recipe::GeneratedData;
using ObjectSource = jak1_output_recipe::ObjectSource;
using ObjectEntry = jak1_output_recipe::ObjectEntry;
using ArchiveRecord = jak1_output_recipe::ArchiveRecord;
using FlatFileCopy = jak1_output_recipe::FlatFileCopy;
using GeneratedFlatFileKind = jak1_output_recipe::GeneratedFlatFileKind;
using GeneratedFlatFile = jak1_output_recipe::GeneratedFlatFile;
using Recipe = jak1_output_recipe::Recipe;
using Limits = jak1_output_recipe::Limits;
using CancelCallback = jak1_output_recipe::CancelCallback;
using ErrorCode = jak1_output_recipe::ErrorCode;
using Error = jak1_output_recipe::Error;

template <typename T>
using Result = jak1_output_recipe::Result<T>;

struct Options {
  Limits limits;
  CancelCallback should_cancel;

  Options();
};

inline constexpr SourceObjectPackIdentity kRecordedSourceObjectPack = {
    jak2_source_object_pack::kExpectedObjectCount,
    jak2_source_object_pack::kRecordedAggregateXXH64,
};

RevisionProvenance revision_provenance(const jak2_iso::Revision& revision);
Recipe make_base_retail_recipe(const jak2_iso::Revision& revision);

Result<std::vector<std::uint8_t>> encode(const Recipe& recipe,
                                         const jak2_iso::Revision& revision,
                                         const Options& options = {});
Result<Recipe> decode(std::span<const std::uint8_t> bytes,
                      const jak2_iso::Revision& revision,
                      const Options& options = {});

inline const char* error_code_name(ErrorCode code) {
  return jak1_output_recipe::error_code_name(code);
}

}  // namespace jak2_output_recipe
