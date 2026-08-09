#include "Jak2OutputMaterializer.h"

#include "common/custom_data/Jak2PublicOutputGraph.h"

namespace jak2_output_materializer {
namespace {

bool supported_revision(const jak2_iso::Revision& revision) {
  const auto& expected = jak2_iso::import_revision();
  return revision.serial == expected.serial && revision.elf_hash == expected.elf_hash &&
         revision.contents_hash == expected.contents_hash &&
         revision.file_count == expected.file_count &&
         revision.decomp_config_version == expected.decomp_config_version &&
         revision.territory == expected.territory;
}

}  // namespace

Options::Options() {
  recipe_limits.max_archives = jak2_public_output_graph::kArchiveLimit;
}

Result<Summary> materialize(const Inputs& inputs,
                            const std::filesystem::path& destination_root,
                            const jak2_iso::Revision& revision,
                            const Options& options) {
  if (!supported_revision(revision)) {
    return Result<Summary>::failure(
        {ErrorCode::revision_mismatch,
         "Jak II output materialization currently supports only SCUS-97265 NTSC-U v2.",
         {},
         {}});
  }
  jak1_output_materializer::Options core_options;
  core_options.limits = options.limits;
  core_options.recipe_limits = options.recipe_limits;
  core_options.expected_revision = jak2_output_recipe::revision_provenance(revision);
  core_options.expected_source_object_pack = jak2_output_recipe::kRecordedSourceObjectPack;
  core_options.wire_game = jak1_output_recipe::WireGame::jak2;
  core_options.should_cancel = options.should_cancel;
  core_options.on_progress = options.on_progress;
  return jak1_output_materializer::materialize(inputs, destination_root, core_options);
}

}  // namespace jak2_output_materializer
