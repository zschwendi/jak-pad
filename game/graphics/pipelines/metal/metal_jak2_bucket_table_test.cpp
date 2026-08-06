#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#include <cstdio>

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

}  // namespace

int main() {
  using Behavior = metal_renderer::Jak2MetalBucketBehavior;
  const auto& table = metal_renderer::jak2_metal_bucket_table();

  std::size_t deferred = 0;
  std::size_t strict_empty = 0;
  std::size_t direct = 0;
  std::size_t host_texture_upload = 0;
  std::size_t visibility = 0;
  std::size_t sprite = 0;
  std::size_t tfragment = 0;
  std::size_t shrub = 0;
  std::size_t tie = 0;
  std::size_t tie_envmap = 0;
  bool contiguous = true;
  for (std::size_t i = 0; i < table.size(); i++) {
    contiguous &= table[i].id == i;
    deferred += table[i].behavior == Behavior::DeferredSkip;
    strict_empty += table[i].behavior == Behavior::StrictEmpty;
    direct += table[i].behavior == Behavior::Direct;
    host_texture_upload += table[i].behavior == Behavior::HostTextureUpload;
    visibility += table[i].behavior == Behavior::Visibility;
    sprite += table[i].behavior == Behavior::Sprite;
    tfragment += table[i].behavior == Behavior::TFragment;
    shrub += table[i].behavior == Behavior::Shrub;
    tie += table[i].behavior == Behavior::Tie;
    tie_envmap += table[i].behavior == Behavior::TieEnvmap;
  }

  check(table.size() == 327 && contiguous, "the Jak 2 table covers 327 contiguous bucket IDs");
  check(deferred == 169, "169 OpenGL-bound buckets remain deferred for Metal");
  check(strict_empty == 127, "127 unbound buckets use strict-empty descriptor policy");
  check(direct == 3, "three reviewed OpenGL-bound buckets are implemented by Metal Direct");
  check(host_texture_upload == 2,
        "two exact texture-upload buckets are executed synchronously by the host");
  check(visibility == 1, "one non-draw visibility bucket owns shared frame data");
  check(sprite == 1, "one normal Sprite3 bucket is implemented by Metal");
  check(tfragment == 6, "six normal per-level TFRAG buckets are implemented by Metal");
  check(shrub == 6, "six normal per-level SHRUB buckets are implemented by Metal");
  check(tie == 6, "six normal per-level TIE parent buckets are implemented by Metal");
  check(tie_envmap == 6,
        "six normal per-level ETIE child buckets are implemented by Metal");
  check(metal_renderer::jak2_metal_bucket_table_fingerprint() ==
            metal_renderer::kJak2MetalBucketExpectedFingerprint,
        "the ordered descriptor policy matches its fixed reference fingerprint");

  check(has_behavior(jak2::BucketId::BUCKET_2, Behavior::Visibility),
        "BUCKET_2 is the explicit non-draw visibility-state bucket");
  check(has_behavior(jak2::BucketId::TFRAG_L0_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L1_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L2_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L3_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L4_TFRAG, Behavior::TFragment) &&
            has_behavior(jak2::BucketId::TFRAG_L5_TFRAG, Behavior::TFragment),
        "all six normal TFRAG level buckets use the explicit Metal TFragment policy");
  check(has_behavior(jak2::BucketId::TEX_L0_TFRAG, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::TFRAG_S_L0_TFRAG, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::TFRAG_T_L0_ALPHA, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::TFRAG_W_L0_WATER, Behavior::DeferredSkip),
        "texture, scissor, translucent, and water TFRAG neighbors remain unpromoted");
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
            has_behavior(jak2::BucketId::TIE_T_L0_ALPHA, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::ETIE_T_L0_ALPHA, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::TIE_W_L0_WATER, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::ETIE_W_L0_WATER, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::MERC_L0_TFRAG, Behavior::DeferredSkip),
        "TIE scissor/vanish remain unbound while translucent, water, and Merc stay deferred");
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
  check(has_behavior(jak2::BucketId::TEX_L0_SHRUB, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::SHRUB_N_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::BILLBOARD_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::SHRUB_V_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::SHRUB_NT_L0_SHRUB, Behavior::StrictEmpty) &&
            has_behavior(jak2::BucketId::MERC_L0_SHRUB, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_SHRUB, Behavior::DeferredSkip),
        "SHRUB texture and every neighboring family remain deferred or strict-empty");
  check(has_behavior(jak2::BucketId::OCEAN_MID_FAR, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_TFRAG, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_SHRUB, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_ALPHA, Behavior::DeferredSkip),
        "ocean and representative unimplemented level families remain deferred");
  check(has_behavior(jak2::BucketId::TEX_LCOM_SKY_PRE, Behavior::HostTextureUpload),
        "TEX_LCOM_SKY_PRE is the explicit host texture-upload bucket");
  check(has_behavior(jak2::BucketId::TEX_ALL_SPRITE, Behavior::HostTextureUpload) &&
            has_behavior(jak2::BucketId::PARTICLES, Behavior::Sprite),
        "the title sprite texture upload and Sprite3 draw buckets are explicit");
  check(has_behavior(jak2::BucketId::SHADOW, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::GMERC_L5_PRIS2, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::ETIE_W_L5_WATER, Behavior::DeferredSkip) &&
            has_behavior(jak2::BucketId::DEBUG3, Behavior::DeferredSkip),
        "common, prismatic, water, and tail bindings are deferred");
  check(has_behavior(jak2::BucketId::SKY_DRAW, Behavior::Direct) &&
            has_behavior(jak2::BucketId::SCREEN_FILTER, Behavior::Direct) &&
            has_behavior(jak2::BucketId::DEBUG_NO_ZBUF2, Behavior::Direct),
        "SKY_DRAW, SCREEN_FILTER, and DEBUG_NO_ZBUF2 are the explicit Direct buckets");
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
  const auto deferred_id = static_cast<std::size_t>(jak2::BucketId::OCEAN_MID_FAR);
  check(!metal_renderer::jak2_metal_bucket_allows_content(strict_id),
        "strict-empty policy does not allow content");
  check(metal_renderer::jak2_metal_bucket_allows_content(deferred_id),
        "deferred policy allows content for later implementation");
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
            static_cast<std::size_t>(jak2::BucketId::SKY_DRAW)),
        "the promoted SKY_DRAW Direct policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)),
        "implemented Direct policy allows content");
  check(metal_renderer::jak2_metal_bucket_allows_content(
            static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)),
        "the promoted DEBUG_NO_ZBUF2 Direct policy allows content");
  check(!metal_renderer::jak2_metal_bucket_allows_content(table.size()),
        "policy rejects an out-of-range bucket ID");

  std::size_t direct_batches = 0;
  for (std::size_t i = 0; i < table.size(); i++) {
    direct_batches += metal_renderer::jak2_metal_direct_batch_size(i) != 0;
  }
  check(direct_batches == 3 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::SKY_DRAW)) == 1024 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)) == 256 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)) == 0x8000 &&
            metal_renderer::jak2_metal_direct_batch_size(
                static_cast<std::size_t>(jak2::BucketId::DEBUG3)) == 0,
        "only the three Direct bindings receive their OpenGL reference batch sizes");
  check(metal_renderer::jak2_metal_direct_batch_size(table.size()) == 0,
        "out-of-range buckets do not receive a Direct binding");

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal bucket table checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 Metal descriptor policy matches the 327-entry OpenGL reference\n");
  return 0;
}
