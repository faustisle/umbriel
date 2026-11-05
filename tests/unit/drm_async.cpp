#include "server/drm_async.h"

#include "check.h"

#include <cstdint>
#include <vector>

using umbriel::filterPreferredModifiers;

namespace {
  constexpr uint64_t ccs = 0x100000000000004ULL;
  constexpr uint64_t yTiled = 0x2;
  constexpr uint64_t xTiled = 0x1;
} // namespace

UMBRIEL_TEST(preferredModifiersSelectOnlyAsyncCapableEntries) {
  const std::vector<uint64_t> supported{ccs, yTiled, xTiled};
  const std::vector<uint64_t> async{yTiled, xTiled};
  CHECK(filterPreferredModifiers(supported.data(), supported.size(), async) == async);
}

UMBRIEL_TEST(preferredModifiersFollowTheDriverAsyncOrder) {
  const std::vector<uint64_t> supported{xTiled, yTiled};
  const std::vector<uint64_t> async{yTiled, xTiled};
  CHECK(filterPreferredModifiers(supported.data(), supported.size(), async) == async);
}

UMBRIEL_TEST(preferredModifiersEmptyWhenNothingOverlaps) {
  const std::vector<uint64_t> supported{ccs};
  const std::vector<uint64_t> async{yTiled};
  CHECK(filterPreferredModifiers(supported.data(), supported.size(), async).empty());
}

UMBRIEL_TEST(preferredModifiersEmptyWithoutPreference) {
  const std::vector<uint64_t> supported{ccs, yTiled};
  CHECK(filterPreferredModifiers(supported.data(), supported.size(), {}).empty());
}

UMBRIEL_TEST(preferredModifiersHandleEmptyInput) {
  const std::vector<uint64_t> async{yTiled};
  CHECK(filterPreferredModifiers(nullptr, 0, async).empty());
}

int main() { return RUN_TESTS(); }
