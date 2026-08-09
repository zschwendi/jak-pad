#include "Jak2OutputRecipe.h"

#include "common/custom_data/Jak2PublicOutputGraph.h"

namespace jak2_output_recipe {
namespace {

bool supported_revision(const jak2_iso::Revision& revision) {
  const auto& expected = jak2_iso::import_revision();
  return revision.serial == expected.serial && revision.elf_hash == expected.elf_hash &&
         revision.contents_hash == expected.contents_hash &&
         revision.file_count == expected.file_count &&
         revision.decomp_config_version == expected.decomp_config_version &&
         revision.territory == expected.territory;
}

jak1_output_recipe::Options core_options(const jak2_iso::Revision& revision,
                                         const Options& options) {
  jak1_output_recipe::Options result;
  result.limits = options.limits;
  result.expected_revision = revision_provenance(revision);
  result.expected_source_object_pack = kRecordedSourceObjectPack;
  result.wire_game = jak1_output_recipe::WireGame::jak2;
  result.should_cancel = options.should_cancel;
  return result;
}

Error unsupported_revision_error() {
  return {ErrorCode::unsupported_revision,
          0,
          {},
          {},
          "Jak II output recipes currently support only SCUS-97265 NTSC-U v2."};
}

}  // namespace

Options::Options() {
  limits.max_archives = jak2_public_output_graph::kArchiveLimit;
}

RevisionProvenance revision_provenance(const jak2_iso::Revision& revision) {
  RevisionProvenance result;
  result.serial = revision.serial;
  result.executable_hash = revision.elf_hash;
  result.contents_hash = revision.contents_hash;
  result.file_count = revision.file_count;
  result.config_version = revision.decomp_config_version;
  result.territory = static_cast<jak1_iso::Territory>(revision.territory);
  result.black_label = false;
  return result;
}

Recipe make_base_retail_recipe(const jak2_iso::Revision& revision) {
  Recipe recipe;
  recipe.producer = jak1_output_recipe::kJak2ProvenanceId;
  recipe.game = jak1_output_recipe::kJak2GameId;
  recipe.profile = OutputProfile::jak2_base_retail;
  recipe.revision = revision_provenance(revision);
  recipe.source_object_pack = kRecordedSourceObjectPack;
  return recipe;
}

Result<std::vector<std::uint8_t>> encode(const Recipe& recipe,
                                         const jak2_iso::Revision& revision,
                                         const Options& options) {
  if (!supported_revision(revision)) {
    return Result<std::vector<std::uint8_t>>::failure(unsupported_revision_error());
  }
  return jak1_output_recipe::encode(recipe, core_options(revision, options));
}

Result<Recipe> decode(std::span<const std::uint8_t> bytes,
                      const jak2_iso::Revision& revision,
                      const Options& options) {
  if (!supported_revision(revision)) {
    return Result<Recipe>::failure(unsupported_revision_error());
  }
  return jak1_output_recipe::decode(bytes, core_options(revision, options));
}

}  // namespace jak2_output_recipe
