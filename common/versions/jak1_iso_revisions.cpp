#include "jak1_iso_revisions.h"

#include <array>

namespace jak1_iso {
namespace {

constexpr std::array<Revision, 5> kSupportedRevisions = {{
    {
        "SCUS-97124",
        7280758013604870207U,
        "Jak & Daxter™: The Precursor Legacy (Black Label)",
        Territory::scea,
        337,
        11363853835861842434U,
        "ntsc_v1",
        true,
    },
    {
        "SCUS-97124",
        744661860962747854U,
        "Jak & Daxter™: The Precursor Legacy",
        Territory::scea,
        338,
        8538304367812415885U,
        "ntsc_v2",
        false,
    },
    {
        "SCES-50361",
        12150718117852276522U,
        "Jak & Daxter™: The Precursor Legacy",
        Territory::scee,
        338,
        16850370297611763875U,
        "pal",
        false,
    },
    {
        "SCPS-15021",
        16909372048085114219U,
        "ジャックＸダクスター　～　旧世界の遺産",
        Territory::scei,
        338,
        1262350561338887717U,
        "jp",
        false,
    },
    {
        "SCPS-56003",
        7280758013604870207U,
        "Jak & Daxter: 구세계의 유산",
        Territory::scea,
        338,
        13924540661438229398U,
        "ntsc_v1",
        false,
    },
}};

}  // namespace

std::span<const Revision> supported_revisions() {
  return kSupportedRevisions;
}

const Revision& default_revision() {
  return kSupportedRevisions.front();
}

}  // namespace jak1_iso
