#pragma once

#include <string_view>

#include "goalc/make/Jak1OutputRecipeGenerator.h"

class MakeSystem;

namespace jak1_output_recipe_generator {

Result<Graph> inspect_make_system(const MakeSystem& make_system, const Options& options = {});

Result<jak1_output_recipe::Recipe> generate(const MakeSystem& make_system,
                                            std::string_view source_object_pack_manifest,
                                            const VerifiedInputs& verified_inputs,
                                            const Options& options = {});

}  // namespace jak1_output_recipe_generator
