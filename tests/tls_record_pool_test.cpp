#include "services/tls_record_pool.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>

int main() {
  services::net::TlsRecordPool<17408, 16384> pool;
  assert(pool.available() == 2);
  assert(!pool.allocate(16383));
  assert(!pool.allocate(17409));
  void* input = pool.allocate(16717);
  void* output = pool.allocate(16717);
  assert(input && output && input != output);
  assert(reinterpret_cast<uintptr_t>(input) % alignof(std::max_align_t) == 0);
  assert(reinterpret_cast<uintptr_t>(output) % alignof(std::max_align_t) == 0);
  assert(pool.available() == 0 && !pool.allocate(16717));
  std::memset(input, 0xa5, 16717);
  std::memset(output, 0x5a, 16717);
  assert(pool.release(input));
  assert(pool.available() == 1);
  for (size_t i = 0; i < 17408; ++i) assert(static_cast<unsigned char*>(input)[i] == 0);
  for (size_t i = 0; i < 16717; ++i) assert(static_cast<unsigned char*>(output)[i] == 0x5a);
  assert(pool.allocate(17408) == input);
  assert(pool.release(input));
  assert(pool.release(output));
  int foreign = 0;
  assert(!pool.release(&foreign) && !pool.release(nullptr));
  for (unsigned i = 0; i < 1500; ++i) {
    input = pool.allocate(16717); output = pool.allocate(16717);
    assert(input && output);
    assert(pool.release(output) && pool.release(input));
  }
  assert(pool.available() == 2);
  std::puts("PASS: two aligned TLS records, size bounds, exhaustion, zeroing, isolation,");
  std::puts("      foreign-pointer routing and 1500 full connection lifecycles");
}
