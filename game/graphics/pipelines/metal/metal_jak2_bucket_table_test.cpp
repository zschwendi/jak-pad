#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#include <cstdio>

#include "common/goal_constants.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

bool has_behavior(jak2::BucketId id, metal_renderer::Jak2MetalBucketBehavior behavior) {
  return metal_renderer::jak2_metal_bucket_table()[static_cast<std::size_t>(id)].behavior ==
         behavior;
}

jak2::BucketId level_bucket(jak2::BucketId level_0, jak2::BucketId level_1, int level) {
  return static_cast<jak2::BucketId>(static_cast<int>(level_0) +
                                     (static_cast<int>(level_1) - static_cast<int>(level_0)) *
                                         level);
}

}  // namespace

int main() {
  using Behavior = metal_renderer::Jak2MetalBucketBehavior;
  const auto& table = metal_renderer::jak2_metal_bucket_table();

  std::size_t deferred = 0;
  std::size_t strict_empty = 0;
  std::size_t direct = 0;
  std::size_t host_texture_upload = 0;
  std::size_t host_texture_upload_direct = 0;
  std::size_t visibility = 0;
  std::size_t sprite = 0;
  std::size_t tfragment = 0;
  std::size_t tfragment_trans = 0;
  std::size_t tfragment_water = 0;
  std::size_t shrub = 0;
  std::size_t tie = 0;
  std::size_t tie_envmap = 0;
  std::size_t tie_trans = 0;
  std::size_t tie_trans_envmap = 0;
  std::size_t tie_water = 0;
  std::size_t tie_water_envmap = 0;
  std::size_t merc = 0;
  std::size_t blit_display = 0;
  std::size_t merc_alpha = 0;
  std::size_t merc_water = 0;
  std::size_t generic2 = 0;
  std::size_t ocean_mid_far = 0;
  std::size_t ocean_near = 0;
  std::size_t pris_eye = 0;
  std::size_t common_pris = 0;
  bool contiguous = true;
  for (std::size_t i = 0; i < table.size(); i++) {
    contiguous &= table[i].id == i;
    deferred += table[i].behavior == Behavior::DeferredSkip;
    strict_empty += table[i].behavior == Behavior::StrictEmpty;
    direct += table[i].behavior == Behavior::Direct;
    host_texture_upload += table[i].behavior == Behavior::HostTextureUpload;
    host_texture_upload_direct += table[i].behavior == Behavior::HostTextureUploadDirect;
    visibility += table[i].behavior == Behavior::Visibility;
    sprite += table[i].behavior == Behavior::Sprite;
    tfragment += table[i].behavior == Behavior::TFragment;
    tfragment_trans += table[i].behavior == Behavior::TFragmentTrans;
    tfragment_water += table[i].behavior == Behavior::TFragmentWater;
    shrub += table[i].behavior == Behavior::Shrub;
    tie += table[i].behavior == Behavior::Tie;
    tie_envmap += table[i].behavior == Behavior::TieEnvmap;
    tie_trans += table[i].behavior == Behavior::TieTrans;
    tie_trans_envmap += table[i].behavior == Behavior::TieTransEnvmap;
    tie_water += table[i].behavior == Behavior::TieWater;
    tie_water_envmap += table[i].behavior == Behavior::TieWaterEnvmap;
    merc += table[i].behavior == Behavior::Merc;
    blit_display += table[i].behavior == Behavior::BlitDisplay;
    merc_alpha += table[i].behavior == Behavior::MercAlpha;
    merc_water += table[i].behavior == Behavior::MercWater;
    generic2 += table[i].behavior == Behavior::Generic2;
    ocean_mid_far += table[i].behavior == Behavior::OceanMidFar;
    ocean_near += table[i].behavior == Behavior::OceanNear;
    pris_eye += table[i].behavior == Behavior::PrisEye;
    common_pris += table[i].behavior == Behavior::CommonPris;
  }

  check(table.size() == 327 && contiguous, "the Jak 2 table covers 327 contiguous bucket IDs");
  check(deferred == 32, "32 OpenGL-bound buckets remain deferred for Metal");
  check(strict_empty == 127, "127 unbound buckets use strict-empty descriptor policy");
  check(direct == 4, "four reviewed OpenGL-bound buckets are implemented by Metal Direct");
  check(host_texture_upload == 28,
        "twenty-eight exact texture/setup buckets are handled synchronously by the host");
  check(host_texture_upload_direct == 2,
        "two exact texture/setup buckets also retain their Direct payloads");
  check(visibility == 1, "one non-draw visibility bucket owns shared frame data");
  check(sprite == 1, "one normal Sprite3 bucket is implemented by Metal");
  check(tfragment == 6, "six normal per-level TFRAG buckets are implemented by Metal");
  check(tfragment_trans == 6,
        "six translucent per-level TFRAG buckets are implemented by Metal");
  check(tfragment_water == 6, "six water per-level TFRAG buckets are implemented by Metal");
  check(shrub == 6, "six normal per-level SHRUB buckets are implemented by Metal");
  check(tie == 6, "six normal per-level TIE parent buckets are implemented by Metal");
  check(tie_envmap == 6,
        "six normal per-level ETIE child buckets are implemented by Metal");
  check(tie_trans == 6, "six translucent per-level TIE child buckets are implemented by Metal");
  check(tie_trans_envmap == 6,
        "six translucent per-level ETIE child buckets are implemented by Metal");
  check(tie_water == 6, "six water per-level TIE child buckets are implemented by Metal");
  check(tie_water_envmap == 6,
        "six water per-level ETIE child buckets are implemented by Metal");
  check(merc == 22,
        "22 normal, shrub, PRIS, and common PRIS Merc buckets are implemented by Metal");
  check(blit_display == 1, "one source-proven BlitDisplays bucket is implemented by Metal");
  check(merc_alpha == 6, "six per-level alpha Merc buckets are implemented by Metal");
  check(merc_water == 7,
        "six per-level and one common water Merc buckets are implemented by Metal");
  check(common_pris == 1, "one common PRIS texture bucket has a dedicated exact renderer");
  check(generic2 == 26,
        "26 source-proven normal GMerc buckets are implemented by Metal Generic2");
  check(ocean_mid_far == 1 && ocean_near == 1,
        "both source-proven OCEAN renderer policies are explicit");
  check(pris_eye == 7,
        "six per-level PRIS buckets and exact PRIS2 bucket 228 use planned eye execution");
  check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
            metal_renderer::kJak2MetalBucketExpectedFingerprint,
        "the ordered descriptor policy matches its fixed reference fingerprint");

  check(has_behavior(jak2::BucketId::BUCKET_2, Behavior::Visibility),
        "BUCKET_2 is the explicit non-draw visibility-state bucket");
  check(has_behavior(jak2::BucketId::BUCKET_3, Behavior::BlitDisplay),
        "BUCKET_3 owns the Jak II framebuffer snapshot and clear semantics");
  check(has_behavior(jak2::BucketId::TFRAG_L0_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L1_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L2_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L3_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L4_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L5_TFRAG, Behavior::TFragment),
        "all six normal TFRAG level buckets use the explicit Metal TFragment policy");
  check(has_behavior(jak2::BucketId::TEX_L0_TFRAG, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L1_TFRAG, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L2_TFRAG, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L3_TFRAG, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L4_TFRAG, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L5_TFRAG, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TFRAG_S_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::TFRAG_T_L0_ALPHA, Behavior::TFragmentTrans) &&
            has_behavior(jak2::BucketId::TFRAG_W_L0_WATER, Behavior::TFragmentWater),
        "normal, translucent, and water TFRAG families are explicit");
  check(has_behavior(jak2::BucketId::TIE_L0_TFRAG, Behavior::Tie) &&
            has_behavior(jak2::BucketId::TIE_L1_TFRAG, Behavior::Tie) &&
            has_behavior(jak2::BucketId::TIE_L2_TFRAG, Behavior::Tie) &&
            has_behavior(jak2::BucketId::TIE_L3_TFRAG, Behavior::Tie) &&
            has_behavior(jak2::BucketId::TIE_L4_TFRAG, Behavior::Tie) &&
            has_behavior(jak2::BucketId::TIE_L5_TFRAG, Behavior::Tie),
        "all six normal TIE parent buckets use the explicit Metal Tie policy");
  check(has_behavior(jak2::BucketId::ETIE_L0_TFRAG, Behavior::TieEnvmap) &&
            has_behavior(jak2::BucketId::ETIE_L1_TFRAG, Behavior::TieEnvmap) &&
            has_behavior(jak2::BucketId::ETIE_L2_TFRAG, Behavior::TieEnvmap) &&
            has_behavior(jak2::BucketId::ETIE_L3_TFRAG, Behavior::TieEnvmap) &&
            has_behavior(jak2::BucketId::ETIE_L4_TFRAG, Behavior::TieEnvmap) &&
            has_behavior(jak2::BucketId::ETIE_L5_TFRAG, Behavior::TieEnvmap),
        "all six normal ETIE child buckets use the explicit Metal TieEnvmap policy");
  check(static_cast<std::size_t>(jak2::BucketId::TIE_L0_TFRAG) == 9 &&
            static_cast<std::size_t>(jak2::BucketId::TIE_L1_TFRAG) == 20 &&
            static_cast<std::size_t>(jak2::BucketId::TIE_L2_TFRAG) == 31 &&
            static_cast<std::size_t>(jak2::BucketId::TIE_L3_TFRAG) == 42 &&
            static_cast<std::size_t>(jak2::BucketId::TIE_L4_TFRAG) == 53 &&
            static_cast<std::size_t>(jak2::BucketId::TIE_L5_TFRAG) == 64 &&
            static_cast<std::size_t>(jak2::BucketId::ETIE_L0_TFRAG) == 10 &&
            static_cast<std::size_t>(jak2::BucketId::ETIE_L1_TFRAG) == 21 &&
            static_cast<std::size_t>(jak2::BucketId::ETIE_L2_TFRAG) == 32 &&
            static_cast<std::size_t>(jak2::BucketId::ETIE_L3_TFRAG) == 43 &&
            static_cast<std::size_t>(jak2::BucketId::ETIE_L4_TFRAG) == 54 &&
            static_cast<std::size_t>(jak2::BucketId::ETIE_L5_TFRAG) == 65,
        "normal TIE/ETIE retain the audited 9/10 through 64/65 parent-child pairs");
  check(has_behavior(jak2::BucketId::TIE_S_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::ETIE_S_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::TIE_V_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::TIE_T_L0_ALPHA, Behavior::TieTrans) &&
            has_behavior(jak2::BucketId::ETIE_T_L0_ALPHA, Behavior::TieTransEnvmap) &&
            has_behavior(jak2::BucketId::TIE_W_L0_WATER, Behavior::TieWater) &&
            has_behavior(jak2::BucketId::ETIE_W_L0_WATER, Behavior::TieWaterEnvmap),
        "TIE scissor/vanish stay unbound while translucent and water are explicit");
  check(has_behavior(jak2::BucketId::MERC_L0_TFRAG, Behavior::Merc) &&
            has_behavior(jak2::BucketId::MERC_L1_TFRAG, Behavior::Merc) &&
            has_behavior(jak2::BucketId::MERC_L2_TFRAG, Behavior::Merc) &&
            has_behavior(jak2::BucketId::MERC_L3_TFRAG, Behavior::Merc) &&
            has_behavior(jak2::BucketId::MERC_L4_TFRAG, Behavior::Merc) &&
            has_behavior(jak2::BucketId::MERC_L5_TFRAG, Behavior::Merc),
        "all six normal Merc level buckets use the explicit Metal Merc policy");
  check(static_cast<std::size_t>(jak2::BucketId::MERC_L0_TFRAG) == 14 &&
            static_cast<std::size_t>(jak2::BucketId::MERC_L1_TFRAG) == 25 &&
            static_cast<std::size_t>(jak2::BucketId::MERC_L2_TFRAG) == 36 &&
            static_cast<std::size_t>(jak2::BucketId::MERC_L3_TFRAG) == 47 &&
            static_cast<std::size_t>(jak2::BucketId::MERC_L4_TFRAG) == 58 &&
            static_cast<std::size_t>(jak2::BucketId::MERC_L5_TFRAG) == 69,
        "normal Merc keeps the audited bucket IDs 14, 25, 36, 47, 58, and 69");
  check(has_behavior(jak2::BucketId::MERC_L0_ALPHA, Behavior::MercAlpha) &&
            has_behavior(jak2::BucketId::MERC_L1_ALPHA, Behavior::MercAlpha) &&
            has_behavior(jak2::BucketId::MERC_L2_ALPHA, Behavior::MercAlpha) &&
            has_behavior(jak2::BucketId::MERC_L3_ALPHA, Behavior::MercAlpha) &&
            has_behavior(jak2::BucketId::MERC_L4_ALPHA, Behavior::MercAlpha) &&
            has_behavior(jak2::BucketId::MERC_L5_ALPHA, Behavior::MercAlpha),
        "all six alpha Merc level buckets use the source-matched Metal Merc variant");
  check(has_behavior(jak2::BucketId::MERC_L0_WATER, Behavior::MercWater) &&
            has_behavior(jak2::BucketId::MERC_L1_WATER, Behavior::MercWater) &&
            has_behavior(jak2::BucketId::MERC_L2_WATER, Behavior::MercWater) &&
            has_behavior(jak2::BucketId::MERC_L3_WATER, Behavior::MercWater) &&
            has_behavior(jak2::BucketId::MERC_L4_WATER, Behavior::MercWater) &&
            has_behavior(jak2::BucketId::MERC_L5_WATER, Behavior::MercWater) &&
            has_behavior(jak2::BucketId::MERC_LCOM_WATER, Behavior::MercWater),
        "all seven OpenGL-bound water Merc buckets use the source-matched Metal Merc variant");
  check(has_behavior(jak2::BucketId::GMERC_L0_TFRAG, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L1_TFRAG, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L2_TFRAG, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L3_TFRAG, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L4_TFRAG, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L5_TFRAG, Behavior::Generic2) &&
            static_cast<std::size_t>(jak2::BucketId::GMERC_L0_TFRAG) == 16 &&
            static_cast<std::size_t>(jak2::BucketId::GMERC_L1_TFRAG) == 27 &&
            static_cast<std::size_t>(jak2::BucketId::GMERC_L2_TFRAG) == 38 &&
            static_cast<std::size_t>(jak2::BucketId::GMERC_L3_TFRAG) == 49 &&
            static_cast<std::size_t>(jak2::BucketId::GMERC_L4_TFRAG) == 60 &&
            static_cast<std::size_t>(jak2::BucketId::GMERC_L5_TFRAG) == 71,
        "all six audited TFRAG GMerc neighbors use the normal Metal Generic2 path");
  check(has_behavior(jak2::BucketId::GMERC_L0_ALPHA, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L1_ALPHA, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L2_ALPHA, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L3_ALPHA, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L4_ALPHA, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L5_ALPHA, Behavior::Generic2),
        "all six OpenGL-bound alpha GMerc buckets use the Jak 2 Generic2 grammar");
  check(has_behavior(jak2::BucketId::GMERC_L0_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L1_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L2_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L3_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L4_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_L5_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_LCOM_WATER, Behavior::StrictEmpty),
        "per-level water GMerc uses Generic2 while its unbound common neighbor stays strict-empty");
  bool source_proven_foreground_families = true;
  bool remaining_prismatic_families_deferred = true;
  bool per_level_pris_eye_routed = true;
  bool per_level_pris_merc_routed = true;
  for (int level = 0; level < jak2::LEVEL_MAX; ++level) {
    source_proven_foreground_families &=
        has_behavior(level_bucket(jak2::BucketId::MERC_L0_TFRAG,
                                  jak2::BucketId::MERC_L1_TFRAG, level),
                     Behavior::Merc) &&
        has_behavior(level_bucket(jak2::BucketId::GMERC_L0_TFRAG,
                                  jak2::BucketId::GMERC_L1_TFRAG, level),
                     Behavior::Generic2) &&
        has_behavior(level_bucket(jak2::BucketId::MERC_L0_SHRUB,
                                  jak2::BucketId::MERC_L1_SHRUB, level),
                     Behavior::Merc) &&
        has_behavior(level_bucket(jak2::BucketId::GMERC_L0_SHRUB,
                                  jak2::BucketId::GMERC_L1_SHRUB, level),
                     Behavior::Generic2);
    per_level_pris_eye_routed &=
        has_behavior(level_bucket(jak2::BucketId::TEX_L0_PRIS,
                                  jak2::BucketId::TEX_L1_PRIS, level),
                     Behavior::PrisEye);
    remaining_prismatic_families_deferred &=
        has_behavior(level_bucket(jak2::BucketId::GMERC_L0_PRIS,
                                  jak2::BucketId::GMERC_L1_PRIS, level),
                     Behavior::DeferredSkip) &&
        (level == 1 ||
         has_behavior(level_bucket(jak2::BucketId::TEX_L0_PRIS2,
                                   jak2::BucketId::TEX_L1_PRIS2, level),
                      Behavior::DeferredSkip)) &&
        (level == 1 ||
         has_behavior(level_bucket(jak2::BucketId::MERC_L0_PRIS2,
                                   jak2::BucketId::MERC_L1_PRIS2, level),
                      Behavior::DeferredSkip)) &&
        has_behavior(level_bucket(jak2::BucketId::GMERC_L0_PRIS2,
                                  jak2::BucketId::GMERC_L1_PRIS2, level),
                     Behavior::DeferredSkip);
    per_level_pris_merc_routed &=
        has_behavior(level_bucket(jak2::BucketId::MERC_L0_PRIS,
                                  jak2::BucketId::MERC_L1_PRIS, level),
                     Behavior::Merc);
  }
  check(source_proven_foreground_families,
        "all per-level TFRAG and SHRUB Merc/normal-GMerc families are routed");
  check(per_level_pris_merc_routed,
        "all six per-level PRIS Merc draw buckets use the existing Metal Merc grammar");
  check(per_level_pris_eye_routed,
        "all six per-level PRIS texture buckets use the dedicated planned eye renderer");
  check(has_behavior(jak2::BucketId::TEX_L1_PRIS2, Behavior::PrisEye) &&
            static_cast<std::size_t>(jak2::BucketId::TEX_L1_PRIS2) == 228,
        "only exact PRIS2 texture bucket 228 is promoted to the planned eye renderer");
  check(has_behavior(jak2::BucketId::MERC_L1_PRIS2, Behavior::Merc) &&
            static_cast<std::size_t>(jak2::BucketId::MERC_L1_PRIS2) == 229,
        "only exact PRIS2 Merc bucket 229 is promoted to the existing typed Merc renderer");
  check(remaining_prismatic_families_deferred,
        "PRIS GMerc and all PRIS2 buckets except the exact 228/229 pair remain deferred");
  check(has_behavior(jak2::BucketId::MERC_LCOM_TFRAG, Behavior::Merc) &&
            has_behavior(jak2::BucketId::GMERC_LCOM_TFRAG, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::TEX_LCOM_PRIS, Behavior::CommonPris) &&
            has_behavior(jak2::BucketId::MERC_LCOM_PRIS, Behavior::Merc) &&
            has_behavior(jak2::BucketId::GMERC_LCOM_PRIS, Behavior::Generic2),
        "common TFRAG and the owned common PRIS texture/draw paths are routed");
  check(has_behavior(jak2::BucketId::SHRUB_L0_SHRUB, Behavior::Shrub) &&
            has_behavior(jak2::BucketId::SHRUB_L1_SHRUB, Behavior::Shrub) &&
            has_behavior(jak2::BucketId::SHRUB_L2_SHRUB, Behavior::Shrub) &&
            has_behavior(jak2::BucketId::SHRUB_L3_SHRUB, Behavior::Shrub) &&
            has_behavior(jak2::BucketId::SHRUB_L4_SHRUB, Behavior::Shrub) &&
            has_behavior(jak2::BucketId::SHRUB_L5_SHRUB, Behavior::Shrub),
        "all six normal SHRUB level buckets use the explicit Metal Shrub policy");
  check(static_cast<std::size_t>(jak2::BucketId::SHRUB_L0_SHRUB) == 74 &&
            static_cast<std::size_t>(jak2::BucketId::SHRUB_L1_SHRUB) == 83 &&
            static_cast<std::size_t>(jak2::BucketId::SHRUB_L2_SHRUB) == 92 &&
            static_cast<std::size_t>(jak2::BucketId::SHRUB_L3_SHRUB) == 101 &&
            static_cast<std::size_t>(jak2::BucketId::SHRUB_L4_SHRUB) == 110 &&
            static_cast<std::size_t>(jak2::BucketId::SHRUB_L5_SHRUB) == 119,
        "normal SHRUB keeps the exact audited bucket IDs 74, 83, 92, 101, 110, and 119");
  check(has_behavior(jak2::BucketId::TEX_L0_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L1_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L2_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L3_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L4_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::TEX_L5_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::SHRUB_N_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::BILLBOARD_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::SHRUB_V_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::SHRUB_NT_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::MERC_L0_SHRUB, Behavior::Merc) &&
            has_behavior(jak2::BucketId::GMERC_L5_SHRUB, Behavior::Generic2),
        "SHRUB setup stays host-owned while its Merc and normal GMerc draws are implemented");
  check(has_behavior(jak2::BucketId::OCEAN_MID_FAR, Behavior::OceanMidFar) &&
            has_behavior(jak2::BucketId::OCEAN_NEAR, Behavior::OceanNear) &&
            has_behavior(jak2::BucketId::TEX_LCOM_WATER, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::TEX_L5_PRIS, Behavior::PrisEye) &&
            has_behavior(jak2::BucketId::TEX_L5_PRIS2, Behavior::DeferredSkip),
        "the paired OCEAN buckets route together while unsupported water and prismatic uploads "
        "defer");
  check(has_behavior(jak2::BucketId::TEX_LCOM_SKY_PRE, Behavior::HostTextureUpload),
        "TEX_LCOM_SKY_PRE is the explicit host texture-upload bucket");
  check(has_behavior(jak2::BucketId::TEX_LCOM_TFRAG, Behavior::HostTextureUpload),
        "TEX_LCOM_TFRAG is the exact host-owned skull-gem texture bucket");
  check(static_cast<std::size_t>(jak2::BucketId::TEX_LCOM_SHRUB) == 191 &&
            static_cast<std::size_t>(jak2::BucketId::MERC_LCOM_SHRUB) == 192 &&
            has_behavior(jak2::BucketId::TEX_LCOM_SHRUB, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::MERC_LCOM_SHRUB, Behavior::Merc),
        "the common shrub texture and Merc buckets use the source-matched Metal path");
  check(has_behavior(jak2::BucketId::TEX_ALL_SPRITE, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::PARTICLES, Behavior::Sprite),
        "the title sprite texture upload and Sprite3 draw buckets are explicit");
  check(has_behavior(jak2::BucketId::DEBUG_NO_ZBUF1, Behavior::HostTextureUploadDirect) &&
            has_behavior(jak2::BucketId::TEX_ALL_MAP, Behavior::HostTextureUploadDirect),
        "DEBUG_NO_ZBUF1 and TEX_ALL_MAP preserve their reference upload-plus-Direct behavior");
  check(has_behavior(jak2::BucketId::SHADOW, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::MERC_L0_PRIS2, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::MERC_L1_PRIS2, Behavior::Merc) &&
            has_behavior(jak2::BucketId::GMERC_L5_PRIS2, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_WATER, Behavior::Generic2) &&
            has_behavior(jak2::BucketId::GMERC_LCOM_WATER, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::DEBUG3, Behavior::DeferredSkip),
        "bucket 229 and remaining PRIS2, water, common-water, and tail bindings stay explicit");
  check(has_behavior(jak2::BucketId::SKY_DRAW, Behavior::Direct) &&
            has_behavior(jak2::BucketId::PROGRESS, Behavior::Direct) &&
            has_behavior(jak2::BucketId::SCREEN_FILTER, Behavior::Direct) &&
            has_behavior(jak2::BucketId::DEBUG_NO_ZBUF2, Behavior::Direct),
        "SKY_DRAW, PROGRESS, SCREEN_FILTER, and DEBUG_NO_ZBUF2 are the explicit Direct buckets");
  check(static_cast<std::size_t>(jak2::BucketId::SKY_DRAW) == 5,
        "SKY_DRAW retains its verified Jak 2 bucket ID 5");
  check(static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2) == 325,
        "DEBUG_NO_ZBUF2 retains its verified Jak 2 bucket ID 325");

  check(has_behavior(jak2::BucketId::BUCKET_0, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::TFRAG_S_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::EMERC_L0_ALPHA, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::DEPTH_CUE, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::BUCKET_323, Behavior::StrictEmpty),
        "representative unbound buckets are strict-empty");

  const auto strict_id = static_cast<std::size_t>(jak2::BucketId::TFRAG_S_L0_TFRAG);
  const auto ocean_mid_far_id = static_cast<std::size_t>(jak2::BucketId::OCEAN_MID_FAR);
  const auto ocean_near_id = static_cast<std::size_t>(jak2::BucketId::OCEAN_NEAR);
  check(!metal_renderer::jak2_metal_bucket_allows_content(strict_id),
        "strict-empty policy does not allow content");
  check(metal_renderer::jak2_metal_bucket_allows_content(ocean_mid_far_id) &&
            metal_renderer::jak2_metal_bucket_allows_content(ocean_near_id),
        "both promoted OCEAN policies allow their source-shaped mesh grammar");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::TFRAG_L0_TFRAG)),
        "implemented normal TFRAG policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::SHRUB_L0_SHRUB)),
        "implemented normal SHRUB policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::TIE_L0_TFRAG)) &&
            metal_renderer::jak2_metal_bucket_allows_content(
                static_cast<std::size_t>(jak2::BucketId::ETIE_L0_TFRAG)),
        "implemented normal TIE parent and ETIE child policies allow their source shapes");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::MERC_L0_TFRAG)),
        "implemented normal Merc policy allows its source shape");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::MERC_LCOM_SHRUB)),
        "implemented common shrub Merc policy allows its source shape");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::MERC_L0_ALPHA)) &&
            metal_renderer::jak2_metal_bucket_allows_content(
                static_cast<std::size_t>(jak2::BucketId::MERC_L0_WATER)) &&
            metal_renderer::jak2_metal_bucket_allows_content(
                static_cast<std::size_t>(jak2::BucketId::MERC_LCOM_WATER)),
        "implemented alpha and water Merc policies allow their shared source grammar");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::SKY_DRAW)),
        "the promoted SKY_DRAW Direct policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)),
        "implemented Direct policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::PROGRESS)),
        "the promoted PROGRESS Direct policy allows title/menu content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)),
        "the promoted DEBUG_NO_ZBUF2 Direct policy allows content");
  check(!metal_renderer::jak2_metal_bucket_allows_content(table.size()),
        "policy rejects an out-of-range bucket ID");

  std::size_t direct_batches = 0;
  for (std::size_t i = 0; i < table.size(); i++) {
    direct_batches += metal_renderer::jak2_metal_direct_batch_size(i) != 0;
  }
  check(direct_batches == 6 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::SKY_DRAW)) == 1024 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)) == 256 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::PROGRESS)) == 0x1000 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF1)) == 1024 * 6 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::TEX_ALL_MAP)) == 1024 * 6 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)) == 0x8000 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG3)) == 0,
        "the four Direct and two upload-plus-Direct bindings receive their reference batch sizes");
  check(metal_renderer::jak2_metal_direct_batch_size(table.size()) == 0,
        "out-of-range buckets do not receive a Direct binding");

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal bucket table checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 Metal descriptor policy matches the 327-entry OpenGL reference\n");
  return 0;
}
