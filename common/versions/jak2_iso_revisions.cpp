#include "jak2_iso_revisions.h"

#include <array>

namespace jak2_iso {
namespace {

// These are the same desktop extraction fingerprints maintained in extractor_iso_database().
constexpr std::array<Revision, 5> kSupportedRevisions = {{
    {
        "SCUS-97265",
        18445016742498932084U,
        "Jak II (NTSC-U v1)",
        Territory::scea,
        593,
        3212700152698192932U,
        "ntsc_v1",
    },
    {
        "SCUS-97265",
        18445016742498932084U,
        "Jak II (NTSC-U v2)",
        Territory::scea,
        593,
        18208811100399420450U,
        "ntsc_v1",
    },
    {
        "SCES-51608",
        18188891052467821088U,
        "Jak II: Renegade",
        Territory::scee,
        593,
        15637648662558474533U,
        "pal",
    },
    {
        "SCPS-15057",
        7409991384254810731U,
        "ジャックＸダクスター2",
        Territory::scei,
        593,
        709902535083998969U,
        "jp",
    },
    {
        "SCKA-20010",
        8398029689314218575U,
        "Jak II",
        // Preserve extractor_iso_database(): the existing desktop metadata assigns SCEI here.
        Territory::scei,
        593,
        7504500484091955379U,
        "ko",
    },
}};

constexpr std::size_t kImportRevisionIndex = 1;

}  // namespace

std::span<const Revision> supported_revisions() {
  return kSupportedRevisions;
}

const Revision& default_revision() {
  return kSupportedRevisions.front();
}

const Revision& import_revision() {
  return kSupportedRevisions[kImportRevisionIndex];
}

}  // namespace jak2_iso
