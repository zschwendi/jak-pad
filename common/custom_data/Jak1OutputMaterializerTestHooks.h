#pragma once

#if defined(__APPLE__) && defined(JAK1_OUTPUT_MATERIALIZER_ENABLE_TEST_HOOKS)

#include <sys/acl.h>

namespace jak1_output_materializer::testing {

using ExtendedAclSetter = int (*)(int descriptor, acl_t acl, acl_type_t type);

ExtendedAclSetter replace_extended_acl_setter(ExtendedAclSetter setter);
bool normalize_cloned_output(int descriptor);

}  // namespace jak1_output_materializer::testing

#endif
