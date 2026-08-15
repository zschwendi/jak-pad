#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

using BucketId = jak2::BucketId;
using Table = std::array<Jak2MetalBucketDescriptor, kJak2MetalBucketCount>;

constexpr std::size_t index(BucketId id) {
  return static_cast<std::size_t>(id);
}

constexpr BucketId level_bucket(BucketId level_0, BucketId level_1, int level) {
  return static_cast<BucketId>(static_cast<int>(level_0) +
                               (static_cast<int>(level_1) - static_cast<int>(level_0)) * level);
}

constexpr Table make_table() {
  Table table = {};
  for (std::size_t i = 0; i < table.size(); i++) {
    table[i] = {static_cast<std::uint16_t>(i), Jak2MetalBucketBehavior::StrictEmpty};
  }

  const auto defer = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::DeferredSkip;
  };
  const auto direct = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Direct;
  };
  const auto host_texture_upload = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::HostTextureUpload;
  };
  const auto host_texture_upload_direct = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::HostTextureUploadDirect;
  };
  const auto visibility = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Visibility;
  };
  const auto sprite = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Sprite;
  };
  const auto tfragment = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TFragment;
  };
  const auto tfragment_trans = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TFragmentTrans;
  };
  const auto tfragment_water = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TFragmentWater;
  };
  const auto shrub = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Shrub;
  };
  const auto tie = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Tie;
  };
  const auto tie_envmap = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TieEnvmap;
  };
  const auto tie_trans = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TieTrans;
  };
  const auto tie_trans_envmap = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TieTransEnvmap;
  };
  const auto tie_water = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TieWater;
  };
  const auto tie_water_envmap = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::TieWaterEnvmap;
  };
  const auto merc = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Merc;
  };
  const auto merc_alpha = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::MercAlpha;
  };
  const auto merc_water = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::MercWater;
  };
  const auto generic2 = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Generic2;
  };
  const auto blit_display = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::BlitDisplay;
  };
  const auto ocean_mid_far = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::OceanMidFar;
  };
  const auto ocean_near = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::OceanNear;
  };
  const auto pris_eye = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::PrisEye;
  };
  const auto common_pris = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::CommonPris;
  };
  const auto effects_lightning = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::EffectsLightning;
  };
  const auto warp = [&table](BucketId id) {
    table[index(id)].behavior = Jak2MetalBucketBehavior::Warp;
  };
  // Mirror every renderer explicitly installed by OpenGLRenderer::init_bucket_renderers_jak2.
  visibility(BucketId::BUCKET_2);
  blit_display(BucketId::BUCKET_3);
  host_texture_upload(BucketId::TEX_LCOM_SKY_PRE);
  direct(BucketId::SKY_DRAW);
  ocean_mid_far(BucketId::OCEAN_MID_FAR);

  for (int level = 0; level < jak2::LEVEL_MAX; level++) {
    host_texture_upload(level_bucket(BucketId::TEX_L0_TFRAG, BucketId::TEX_L1_TFRAG, level));
    tfragment(level_bucket(BucketId::TFRAG_L0_TFRAG, BucketId::TFRAG_L1_TFRAG, level));
    tie(level_bucket(BucketId::TIE_L0_TFRAG, BucketId::TIE_L1_TFRAG, level));
    tie_envmap(level_bucket(BucketId::ETIE_L0_TFRAG, BucketId::ETIE_L1_TFRAG, level));
    merc(level_bucket(BucketId::MERC_L0_TFRAG, BucketId::MERC_L1_TFRAG, level));
    generic2(level_bucket(BucketId::GMERC_L0_TFRAG, BucketId::GMERC_L1_TFRAG, level));

    host_texture_upload(level_bucket(BucketId::TEX_L0_SHRUB, BucketId::TEX_L1_SHRUB, level));
    shrub(level_bucket(BucketId::SHRUB_L0_SHRUB, BucketId::SHRUB_L1_SHRUB, level));
    merc(level_bucket(BucketId::MERC_L0_SHRUB, BucketId::MERC_L1_SHRUB, level));
    generic2(level_bucket(BucketId::GMERC_L0_SHRUB, BucketId::GMERC_L1_SHRUB, level));

    host_texture_upload(level_bucket(BucketId::TEX_L0_ALPHA, BucketId::TEX_L1_ALPHA, level));
    tfragment_trans(level_bucket(BucketId::TFRAG_T_L0_ALPHA, BucketId::TFRAG_T_L1_ALPHA, level));
    tie_trans(level_bucket(BucketId::TIE_T_L0_ALPHA, BucketId::TIE_T_L1_ALPHA, level));
    tie_trans_envmap(level_bucket(BucketId::ETIE_T_L0_ALPHA, BucketId::ETIE_T_L1_ALPHA, level));
    merc_alpha(level_bucket(BucketId::MERC_L0_ALPHA, BucketId::MERC_L1_ALPHA, level));
    generic2(level_bucket(BucketId::GMERC_L0_ALPHA, BucketId::GMERC_L1_ALPHA, level));

    pris_eye(level_bucket(BucketId::TEX_L0_PRIS, BucketId::TEX_L1_PRIS, level));
    merc(level_bucket(BucketId::MERC_L0_PRIS, BucketId::MERC_L1_PRIS, level));
    defer(level_bucket(BucketId::GMERC_L0_PRIS, BucketId::GMERC_L1_PRIS, level));

    pris_eye(level_bucket(BucketId::TEX_L0_PRIS2, BucketId::TEX_L1_PRIS2, level));
    merc(level_bucket(BucketId::MERC_L0_PRIS2, BucketId::MERC_L1_PRIS2, level));
    defer(level_bucket(BucketId::GMERC_L0_PRIS2, BucketId::GMERC_L1_PRIS2, level));

    host_texture_upload(level_bucket(BucketId::TEX_L0_WATER, BucketId::TEX_L1_WATER, level));
    merc_water(level_bucket(BucketId::MERC_L0_WATER, BucketId::MERC_L1_WATER, level));
    generic2(level_bucket(BucketId::GMERC_L0_WATER, BucketId::GMERC_L1_WATER, level));
    tfragment_water(level_bucket(BucketId::TFRAG_W_L0_WATER, BucketId::TFRAG_W_L1_WATER, level));
    tie_water(level_bucket(BucketId::TIE_W_L0_WATER, BucketId::TIE_W_L1_WATER, level));
    tie_water_envmap(
        level_bucket(BucketId::ETIE_W_L0_WATER, BucketId::ETIE_W_L1_WATER, level));
  }
  host_texture_upload(BucketId::TEX_LCOM_TFRAG);
  merc(BucketId::MERC_LCOM_TFRAG);
  host_texture_upload(BucketId::TEX_LCOM_SHRUB);
  merc(BucketId::MERC_LCOM_SHRUB);
  generic2(BucketId::GMERC_LCOM_TFRAG);
  defer(BucketId::SHADOW);
  common_pris(BucketId::TEX_LCOM_PRIS);
  merc(BucketId::MERC_LCOM_PRIS);
  generic2(BucketId::GMERC_LCOM_PRIS);
  host_texture_upload(BucketId::TEX_LCOM_WATER);
  merc_water(BucketId::MERC_LCOM_WATER);
  host_texture_upload(BucketId::TEX_LCOM_SKY_POST);
  ocean_near(BucketId::OCEAN_NEAR);
  host_texture_upload(BucketId::TEX_ALL_SPRITE);
  sprite(BucketId::PARTICLES);
  defer(BucketId::SHADOW2);
  effects_lightning(BucketId::EFFECTS);
  host_texture_upload(BucketId::TEX_ALL_WARP);
  warp(BucketId::GMERC_WARP);
  host_texture_upload_direct(BucketId::DEBUG_NO_ZBUF1);
  host_texture_upload_direct(BucketId::TEX_ALL_MAP);
  direct(BucketId::PROGRESS);
  direct(BucketId::SCREEN_FILTER);
  defer(BucketId::SUBTITLE);
  defer(BucketId::DEBUG2);
  direct(BucketId::DEBUG_NO_ZBUF2);
  defer(BucketId::DEBUG3);

  return table;
}

constexpr std::size_t count_behavior(const Table& table, Jak2MetalBucketBehavior behavior) {
  std::size_t count = 0;
  for (const auto& descriptor : table) {
    if (descriptor.behavior == behavior) {
      count++;
    }
  }
  return count;
}

constexpr std::uint64_t fingerprint(const Table& table) {
  constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
  constexpr std::uint64_t kPrime = 1099511628211ull;
  std::uint64_t result = kOffsetBasis;
  const auto hash_byte = [&result](std::uint8_t value) {
    result ^= value;
    result *= kPrime;
  };
  for (const auto& descriptor : table) {
    hash_byte(static_cast<std::uint8_t>(descriptor.id));
    hash_byte(static_cast<std::uint8_t>(descriptor.id >> 8));
    hash_byte(static_cast<std::uint8_t>(descriptor.behavior));
  }
  return result;
}

constexpr auto kTable = make_table();
constexpr auto kTableFingerprint = fingerprint(kTable);
static_assert(kTable.size() == 327);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::DeferredSkip) == 17);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::StrictEmpty) == 127);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Direct) == 4);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::HostTextureUpload) == 31);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::HostTextureUploadDirect) == 2);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Visibility) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Sprite) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TFragment) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TFragmentTrans) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TFragmentWater) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Shrub) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Tie) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TieEnvmap) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TieTrans) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TieTransEnvmap) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TieWater) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::TieWaterEnvmap) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Merc) == 27);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::BlitDisplay) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::MercAlpha) == 6);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::MercWater) == 7);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Generic2) == 26);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::OceanMidFar) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::OceanNear) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::PrisEye) == 12);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::CommonPris) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::EffectsLightning) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Warp) == 1);
static_assert(count_behavior(kTable, Jak2MetalBucketBehavior::Shadow2) == 0);
static_assert(kTableFingerprint == kJak2MetalBucketExpectedFingerprint);

}  // namespace

const std::array<Jak2MetalBucketDescriptor, kJak2MetalBucketCount>& jak2_metal_bucket_table() {
  return kTable;
}

std::uint64_t jak2_metal_bucket_table_fingerprint() {
  return kTableFingerprint;
}

bool jak2_metal_bucket_allows_content(std::size_t bucket_id) {
  return bucket_id < kTable.size() &&
         kTable[bucket_id].behavior != Jak2MetalBucketBehavior::StrictEmpty;
}

int jak2_metal_direct_batch_size(std::size_t bucket_id) {
  if (bucket_id >= kTable.size() ||
      (kTable[bucket_id].behavior != Jak2MetalBucketBehavior::Direct &&
       kTable[bucket_id].behavior != Jak2MetalBucketBehavior::HostTextureUploadDirect)) {
    return 0;
  }
  switch (static_cast<BucketId>(bucket_id)) {
    case BucketId::SKY_DRAW:
      return 1024;
    case BucketId::SCREEN_FILTER:
      return 256;
    case BucketId::PROGRESS:
      return 0x1000;
    case BucketId::DEBUG_NO_ZBUF1:
    case BucketId::TEX_ALL_MAP:
      return 1024 * 6;
    case BucketId::DEBUG_NO_ZBUF2:
      return 0x8000;
    default:
      return 0;
  }
}

}  // namespace metal_renderer
