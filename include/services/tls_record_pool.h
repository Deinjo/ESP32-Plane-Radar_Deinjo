#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace services::net {

// Two permanently reserved record buffers for ONE TLS connection. All smaller
// allocations continue through the SDK allocator. Locking is owned by the port.
template <size_t RecordBytes, size_t MinimumRecordBytes>
class TlsRecordPool {
 public:
  bool accepts(size_t bytes) const {
    return bytes >= MinimumRecordBytes && bytes <= RecordBytes;
  }

  void* allocate(size_t bytes) {
    if (!accepts(bytes)) return nullptr;
    for (size_t i = 0; i < 2; ++i) {
      if (!used_[i]) {
        used_[i] = true;
        std::memset(records_[i], 0, RecordBytes);
        return records_[i];
      }
    }
    return nullptr;
  }

  bool release(void* pointer) {
    for (size_t i = 0; i < 2; ++i) {
      if (pointer == records_[i]) {
        // Volatile writes prevent the wipe from being optimized away.
        volatile unsigned char* bytes = records_[i];
        for (size_t j = 0; j < RecordBytes; ++j) bytes[j] = 0;
        used_[i] = false;
        return true;
      }
    }
    return false;
  }

  size_t available() const {
    return static_cast<size_t>(!used_[0]) + static_cast<size_t>(!used_[1]);
  }

 private:
  alignas(std::max_align_t) unsigned char records_[2][RecordBytes] = {};
  bool used_[2] = {};
};

bool initTlsRecordPool();
bool tlsRecordPoolIdle();
void printTlsRecordPoolDiagnostics();

}  // namespace services::net
