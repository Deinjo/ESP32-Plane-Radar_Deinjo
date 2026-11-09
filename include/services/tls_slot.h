#pragma once

#include <cstddef>
#include <cstdint>

namespace services::net {

// Use only after releasing the previous TLS owner. Total heap and allocatable
// internal blocks are different constraints; neither can substitute for the other.
inline bool tlsSlotHasReserve(uint32_t free_heap, size_t largest,
                              size_t largest_internal, uint32_t minimum_free,
                              size_t minimum_block) {
  return free_heap >= minimum_free && largest >= minimum_block &&
         largest_internal >= minimum_block;
}

}  // namespace services::net
