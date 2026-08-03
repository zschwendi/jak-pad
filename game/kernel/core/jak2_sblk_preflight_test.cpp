#include "game/kernel/core/sblk_preflight.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using sblk_preflight::Error;

constexpr std::uint32_t kSblk = std::uint32_t('S') | (std::uint32_t('B') << 8) |
                                (std::uint32_t('l') << 16) | (std::uint32_t('k') << 24);
constexpr std::size_t kOuterSize = 24;
constexpr std::size_t kV1HeaderSize = 60;
constexpr std::size_t kV2HeaderSize = 64;
constexpr std::size_t kSoundSize = 12;
constexpr std::size_t kV1GrainSize = 0x28;
constexpr std::size_t kV2GrainSize = 8;

int g_failures = 0;

void check(bool condition, std::string_view message) {
  std::printf("%s %.*s\n", condition ? "PASS" : "FAIL", int(message.size()), message.data());
  g_failures += !condition;
}

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::size_t offset, T value) {
  if (bytes.size() < offset + sizeof(value)) {
    bytes.resize(offset + sizeof(value));
  }
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename T>
T get(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  T value{};
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

struct Fixture {
  std::vector<std::uint8_t> file;
  std::size_t bank = kOuterSize;
  std::size_t samples = 0;
};

Fixture wrap(std::vector<std::uint8_t> bank, std::vector<std::uint8_t> samples = {}) {
  Fixture out;
  out.samples = out.bank + bank.size();
  out.file.resize(out.samples + samples.size());
  put<std::uint32_t>(out.file, 0, 1);
  put<std::uint32_t>(out.file, 4, 2);
  put<std::uint32_t>(out.file, 8, std::uint32_t(out.bank));
  put<std::uint32_t>(out.file, 12, std::uint32_t(bank.size()));
  put<std::uint32_t>(out.file, 16, std::uint32_t(out.samples));
  put<std::uint32_t>(out.file, 20, std::uint32_t(samples.size()));
  std::copy(bank.begin(), bank.end(), out.file.begin() + out.bank);
  std::copy(samples.begin(), samples.end(), out.file.begin() + out.samples);
  return out;
}

std::vector<std::uint8_t> v1_bank(std::uint32_t grain_type = 24) {
  const std::size_t first_sound = kV1HeaderSize;
  const std::size_t first_grain = first_sound + kSoundSize;
  std::vector<std::uint8_t> bank(first_grain + kV1GrainSize);
  put(bank, 0, kSblk);
  put<std::uint32_t>(bank, 4, 1);
  put<std::int16_t>(bank, 22, 1);
  put<std::int16_t>(bank, 24, 1);
  put<std::int16_t>(bank, 26, 0);
  put<std::uint32_t>(bank, 28, std::uint32_t(first_sound));
  put<std::uint32_t>(bank, 32, std::uint32_t(first_grain));
  put<std::int8_t>(bank, first_sound + 4, 1);
  put<std::uint32_t>(bank, first_sound + 8, 0);
  put(bank, first_grain, grain_type);
  if (grain_type == 5 || grain_type == 6 || grain_type == 8) {
    put<std::int32_t>(bank, first_grain + 8 + 12, -1);
  }
  return bank;
}

std::size_t v2_payload_size(std::uint8_t type) {
  switch (type) {
    case 1:
    case 9:
      return 24;
    case 4:
      return 16;
    case 5:
    case 6:
    case 7:
    case 8:
      return 32;
    default:
      return 0;
  }
}

std::vector<std::uint8_t> v2_bank(std::uint8_t grain_type = 1,
                                  std::size_t payload_size = std::numeric_limits<std::size_t>::max()) {
  const std::size_t first_sound = kV2HeaderSize;
  const std::size_t first_grain = first_sound + kSoundSize;
  const std::size_t grain_data = first_grain + kV2GrainSize;
  if (payload_size == std::numeric_limits<std::size_t>::max()) {
    payload_size = v2_payload_size(grain_type);
  }
  std::vector<std::uint8_t> bank(grain_data + payload_size);
  put(bank, 0, kSblk);
  put<std::uint32_t>(bank, 4, 2);
  put<std::int16_t>(bank, 22, 1);
  put<std::int16_t>(bank, 24, 1);
  put<std::int16_t>(bank, 26, 0);
  put<std::uint32_t>(bank, 28, std::uint32_t(first_sound));
  put<std::uint32_t>(bank, 32, std::uint32_t(first_grain));
  put<std::uint32_t>(bank, 52, std::uint32_t(grain_data));
  put<std::int8_t>(bank, first_sound + 4, 1);
  put<std::uint32_t>(bank, first_sound + 8, 0);
  put<std::uint32_t>(bank, first_grain, std::uint32_t(grain_type) << 24);
  if (payload_size >= 16 && (grain_type == 5 || grain_type == 6 || grain_type == 8)) {
    put<std::int32_t>(bank, grain_data + 12, -1);
  }
  return bank;
}

std::vector<std::uint8_t> shared_grain_budget_bank(std::int8_t final_grain_count) {
  constexpr std::size_t sound_count = 259;
  constexpr std::size_t grain_count = 127;
  constexpr std::size_t first_sound = kV2HeaderSize;
  constexpr std::size_t first_grain = first_sound + sound_count * kSoundSize;
  constexpr std::size_t grain_data = first_grain + grain_count * kV2GrainSize;
  std::vector<std::uint8_t> bank(grain_data);
  put(bank, 0, kSblk);
  put<std::uint32_t>(bank, 4, 2);
  put<std::int16_t>(bank, 22, static_cast<std::int16_t>(sound_count));
  put<std::int16_t>(bank, 24, static_cast<std::int16_t>(grain_count));
  put<std::int16_t>(bank, 26, 0);
  put<std::uint32_t>(bank, 28, static_cast<std::uint32_t>(first_sound));
  put<std::uint32_t>(bank, 32, static_cast<std::uint32_t>(first_grain));
  put<std::uint32_t>(bank, 52, static_cast<std::uint32_t>(grain_data));
  for (std::size_t sound = 0; sound < sound_count; sound++) {
    const std::int8_t references =
        sound + 1 == sound_count ? final_grain_count : static_cast<std::int8_t>(grain_count);
    put<std::int8_t>(bank, first_sound + sound * kSoundSize + 4, references);
    put<std::uint32_t>(bank, first_sound + sound * kSoundSize + 8, 0);
  }
  for (std::size_t grain = 0; grain < grain_count; grain++) {
    put<std::uint32_t>(bank, first_grain + grain * kV2GrainSize, std::uint32_t(24) << 24);
  }
  return bank;
}

Error result(const Fixture& fixture) {
  return sblk_preflight::validate(fixture.file).error;
}

void expect(Error expected, const Fixture& fixture, std::string_view message) {
  const Error actual = result(fixture);
  check(actual == expected,
        actual == expected ? message : sblk_preflight::error_name(actual));
}

void expect_valid(const Fixture& fixture, std::string_view message) {
  expect(Error::none, fixture, message);
}

Fixture with_names(bool sentinel = true) {
  auto bank = v1_bank();
  const std::size_t names = bank.size();
  const std::size_t table = names + 0x98;
  bank.resize(table + (sentinel ? 2 : 1) * 0x14);
  put<std::uint32_t>(bank, 8, 0x100);
  put<std::uint32_t>(bank, 52, std::uint32_t(names));
  put<std::uint32_t>(bank, names + 8, 0x98);
  for (std::size_t bucket = 0; bucket < 32; bucket++) {
    put<std::int16_t>(bank, names + 0x18 + bucket * 2, 0);
  }
  put<std::uint32_t>(bank, table, 0x454d414e);
  put<std::int16_t>(bank, table + 0x10, 0);
  return wrap(std::move(bank));
}

void basic_cases() {
  expect_valid(wrap(v1_bank()), "minimal v1 SBlk is accepted");
  expect_valid(wrap(v2_bank(), {0, 0}), "minimal v2 tone SBlk is accepted");

  Fixture short_outer;
  short_outer.file.resize(kOuterSize - 1);
  expect(Error::outer_header, short_outer, "truncated outer header is rejected");

  auto wrong_type = wrap(v1_bank());
  put<std::uint32_t>(wrong_type.file, 0, 2);
  expect(Error::file_type, wrong_type, "unsupported outer type is rejected");
  auto wrong_chunks = wrap(v1_bank());
  put<std::uint32_t>(wrong_chunks.file, 4, 3);
  expect(Error::chunk_count, wrong_chunks, "unexpected outer chunk count is rejected");
  auto chunk_oob = wrap(v1_bank());
  put<std::uint32_t>(chunk_oob.file, 12, std::numeric_limits<std::uint32_t>::max());
  expect(Error::chunk_range, chunk_oob, "outer chunk overflow is rejected");
  auto wrong_tag = wrap(v1_bank());
  put<std::uint32_t>(wrong_tag.file, wrong_tag.bank, 0);
  expect(Error::bank_tag, wrong_tag, "non-SBlk bank is rejected");
  auto short_header = wrap(std::vector<std::uint8_t>(kV1HeaderSize - 1));
  put(short_header.file, short_header.bank, kSblk);
  put<std::uint32_t>(short_header.file, short_header.bank + 4, 1);
  expect(Error::bank_header, short_header, "truncated SBlk header is rejected");
}

void count_and_table_cases() {
  auto negative_sounds = wrap(v1_bank());
  put<std::int16_t>(negative_sounds.file, negative_sounds.bank + 22, -1);
  expect(Error::negative_count, negative_sounds, "negative sound count is rejected");
  auto negative_grains = wrap(v1_bank());
  put<std::int16_t>(negative_grains.file, negative_grains.bank + 24, -1);
  expect(Error::negative_count, negative_grains, "negative global grain count is rejected");
  auto negative_vags = wrap(v1_bank());
  put<std::int16_t>(negative_vags.file, negative_vags.bank + 26, -1);
  expect(Error::negative_count, negative_vags, "negative VAG count is rejected");
  auto sound_oob = wrap(v1_bank());
  put<std::uint32_t>(sound_oob.file, sound_oob.bank + 28, std::numeric_limits<std::uint32_t>::max());
  expect(Error::sound_table, sound_oob, "sound table overflow is rejected");
  auto local_negative = wrap(v1_bank());
  put<std::int8_t>(local_negative.file, local_negative.bank + kV1HeaderSize + 4, -1);
  expect(Error::negative_count, local_negative, "negative per-sound grain count is rejected");
  auto grain_oob = wrap(v1_bank());
  put<std::uint32_t>(grain_oob.file, grain_oob.bank + 32,
                     std::numeric_limits<std::uint32_t>::max());
  expect(Error::grain_table, grain_oob, "grain table overflow is rejected");
  auto grain_relative_oob = wrap(v1_bank());
  put<std::uint32_t>(grain_relative_oob.file,
                     grain_relative_oob.bank + kV1HeaderSize + 8,
                     std::numeric_limits<std::uint32_t>::max());
  expect(Error::grain_table, grain_relative_oob, "per-sound grain offset overflow is rejected");
  auto amplified = wrap(v1_bank());
  put<std::int16_t>(amplified.file, amplified.bank + 24, 0);
  expect(Error::grain_table, amplified, "per-sound grains cannot exceed the declared table");

  constexpr std::size_t shared_first_grain = kV2HeaderSize + 2 * kSoundSize;
  constexpr std::size_t shared_grain_data = shared_first_grain + kV2GrainSize;
  std::vector<std::uint8_t> shared_grain_bank(shared_grain_data);
  put(shared_grain_bank, 0, kSblk);
  put<std::uint32_t>(shared_grain_bank, 4, 2);
  put<std::int16_t>(shared_grain_bank, 22, 2);
  put<std::int16_t>(shared_grain_bank, 24, 1);
  put<std::int16_t>(shared_grain_bank, 26, 0);
  put<std::uint32_t>(shared_grain_bank, 28, kV2HeaderSize);
  put<std::uint32_t>(shared_grain_bank, 32, shared_first_grain);
  put<std::uint32_t>(shared_grain_bank, 52, shared_grain_data);
  put<std::int8_t>(shared_grain_bank, kV2HeaderSize + 4, 1);
  put<std::uint32_t>(shared_grain_bank, kV2HeaderSize + 8, 0);
  put<std::int8_t>(shared_grain_bank, kV2HeaderSize + kSoundSize + 4, 1);
  put<std::uint32_t>(shared_grain_bank, kV2HeaderSize + kSoundSize + 8, 0);
  put<std::uint32_t>(shared_grain_bank, shared_first_grain, std::uint32_t(24) << 24);
  expect_valid(wrap(std::move(shared_grain_bank)),
               "two sounds may share one declared grain range");
  expect_valid(wrap(shared_grain_budget_bank(1)),
               "the format-derived decoded-grain budget is accepted exactly");
  expect(Error::allocation_budget, wrap(shared_grain_budget_bank(2)),
         "shared ranges cannot amplify decoded grain storage past the budget");
}

void grain_cases() {
  auto truncated_v1 = wrap(v1_bank());
  truncated_v1.file.pop_back();
  put<std::uint32_t>(truncated_v1.file, 12,
                     get<std::uint32_t>(truncated_v1.file, 12) - 1);
  put<std::uint32_t>(truncated_v1.file, 16,
                     get<std::uint32_t>(truncated_v1.file, 16) - 1);
  expect(Error::grain_table, truncated_v1, "truncated v1 grain record is rejected");

  auto v1_bad_type = wrap(v1_bank(45));
  expect(Error::grain_type, v1_bad_type, "v1 grain type 45 is rejected");
  expect_valid(wrap(v1_bank(10)), "v1 unknown-handler type 10 remains accepted");
  auto v2_bad_type = wrap(v2_bank(45));
  expect(Error::grain_type, v2_bad_type, "v2 grain type 45 is rejected");
  expect_valid(wrap(v2_bank(10)), "v2 unknown-handler type 10 remains accepted");

  for (const auto [type, bytes, name] :
       std::array<std::tuple<std::uint8_t, std::size_t, const char*>, 4>{{
           {1, 24, "tone"}, {4, 16, "LFO"}, {5, 32, "play-sound"}, {7, 32, "plugin"}}}) {
    auto truncated = wrap(v2_bank(type, bytes - 1), {0, 0});
    expect(type == 1 ? Error::sample_data : Error::grain_data, truncated, name);
  }

  auto v2_bad_payload = wrap(v2_bank(4));
  put<std::uint32_t>(v2_bad_payload.file, v2_bad_payload.bank + kV2HeaderSize + kSoundSize,
                     (std::uint32_t(4) << 24) | 0x00ffffff);
  expect(Error::grain_data, v2_bad_payload, "v2 grain-data offset overflow is rejected");

  auto invalid_lfo = wrap(v2_bank(4));
  const std::size_t lfo = invalid_lfo.bank + kV2HeaderSize + kSoundSize + kV2GrainSize;
  put<std::uint8_t>(invalid_lfo.file, lfo, 4);
  expect(Error::grain_data, invalid_lfo, "out-of-range LFO slot is rejected");

  auto odd_sample = wrap(v2_bank(), {0, 0, 0});
  const std::size_t tone = odd_sample.bank + kV2HeaderSize + kSoundSize + kV2GrainSize;
  put<std::uint32_t>(odd_sample.file, tone + 16, 1);
  expect(Error::sample_data, odd_sample, "unaligned normal tone sample is rejected");
  auto end_sample = wrap(v2_bank(), {0, 0});
  const std::size_t end_tone = end_sample.bank + kV2HeaderSize + kSoundSize + kV2GrainSize;
  put<std::uint32_t>(end_sample.file, end_tone + 16, 2);
  expect(Error::sample_data, end_sample, "normal tone at sample end is rejected");
  auto noise_at_end = end_sample;
  put<std::uint16_t>(noise_at_end.file, end_tone + 14, 8);
  expect_valid(noise_at_end, "noise tone may form a one-past sample pointer without dereferencing");
  auto noise_beyond = noise_at_end;
  put<std::uint32_t>(noise_beyond.file, end_tone + 16, 3);
  expect(Error::sample_data, noise_beyond, "noise tone beyond sample storage is rejected");

  auto invalid_child = wrap(v2_bank(5));
  const std::size_t child_payload = invalid_child.bank + kV2HeaderSize + kSoundSize + kV2GrainSize;
  put<std::int32_t>(invalid_child.file, child_payload + 12, 1);
  expect(Error::child_sound, invalid_child, "out-of-range child sound is rejected");
  auto valid_branch = wrap(v2_bank(8));
  const std::size_t branch_payload = valid_branch.bank + kV2HeaderSize + kSoundSize + kV2GrainSize;
  put<std::int32_t>(valid_branch.file, branch_payload + 12, 0);
  expect_valid(valid_branch, "branch to a populated in-block sound is accepted");

  constexpr std::size_t two_sound_first_grain = kV2HeaderSize + 2 * kSoundSize;
  constexpr std::size_t two_sound_grain_data = two_sound_first_grain + kV2GrainSize;
  std::vector<std::uint8_t> empty_target_bank(two_sound_grain_data + 32);
  put(empty_target_bank, 0, kSblk);
  put<std::uint32_t>(empty_target_bank, 4, 2);
  put<std::int16_t>(empty_target_bank, 22, 2);
  put<std::int16_t>(empty_target_bank, 24, 1);
  put<std::int16_t>(empty_target_bank, 26, 0);
  put<std::uint32_t>(empty_target_bank, 28, kV2HeaderSize);
  put<std::uint32_t>(empty_target_bank, 32, two_sound_first_grain);
  put<std::uint32_t>(empty_target_bank, 52, two_sound_grain_data);
  put<std::int8_t>(empty_target_bank, kV2HeaderSize + 4, 1);
  put<std::uint32_t>(empty_target_bank, kV2HeaderSize + 8, 0);
  put<std::int8_t>(empty_target_bank, kV2HeaderSize + kSoundSize + 4, 0);
  put<std::uint32_t>(empty_target_bank, two_sound_first_grain, std::uint32_t(8) << 24);
  put<std::int32_t>(empty_target_bank, two_sound_grain_data + 12, 1);
  auto empty_branch = wrap(std::move(empty_target_bank));
  expect(Error::child_sound, empty_branch, "branch to a sound without grains is rejected");
}

void optional_table_cases() {
  expect_valid(with_names(), "bounded names bucket is accepted");
  auto negative_hash = with_names();
  const std::size_t names = negative_hash.bank + get<std::uint32_t>(negative_hash.file,
                                                                    negative_hash.bank + 52);
  put<std::int16_t>(negative_hash.file, names + 0x18, -1);
  expect(Error::names, negative_hash, "negative names hash offset is rejected");
  auto bad_hash = with_names();
  const std::size_t bad_names = bad_hash.bank +
                                get<std::uint32_t>(bad_hash.file, bad_hash.bank + 52);
  put<std::int16_t>(bad_hash.file, bad_names + 0x18, 100);
  expect(Error::names, bad_hash, "out-of-range names hash offset is rejected");
  auto unterminated = with_names(false);
  expect(Error::names, unterminated, "unterminated names bucket is rejected");
  auto bad_index = with_names();
  const std::size_t index_names = bad_index.bank +
                                  get<std::uint32_t>(bad_index.file, bad_index.bank + 52);
  const std::size_t index_table = index_names + get<std::uint32_t>(bad_index.file, index_names + 8);
  put<std::int16_t>(bad_index.file, index_table + 0x10, 1);
  expect(Error::names, bad_index, "out-of-range names sound index is rejected");
  auto bad_table = with_names();
  const std::size_t table_names = bad_table.bank +
                                  get<std::uint32_t>(bad_table.file, bad_table.bank + 52);
  put<std::uint32_t>(bad_table.file, table_names + 8,
                     std::numeric_limits<std::uint32_t>::max());
  expect(Error::names, bad_table, "names table offset overflow is rejected");

  auto userdata_bank = v1_bank();
  const std::size_t userdata = userdata_bank.size();
  userdata_bank.resize(userdata + 0x10);
  put<std::uint32_t>(userdata_bank, 8, 0x200);
  put<std::uint32_t>(userdata_bank, 56, std::uint32_t(userdata));
  expect_valid(wrap(userdata_bank), "bounded user-data table is accepted");
  put<std::uint32_t>(userdata_bank, 56, std::uint32_t(userdata + 1));
  expect(Error::user_data, wrap(userdata_bank), "truncated user-data table is rejected");
}

}  // namespace

int main() {
  basic_cases();
  count_and_table_cases();
  grain_cases();
  optional_table_cases();
  std::printf("\n%s: Jak 2 SBlk preflight\n", g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
