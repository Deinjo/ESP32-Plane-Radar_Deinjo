#include "services/tls_record_pool.h"

#include <Arduino.h>
extern "C" {
#include <esp_mem.h>
}
#include <mbedtls/platform.h>
#include <limits>

namespace services::net {
namespace {

constexpr size_t kRecordPayload = MBEDTLS_SSL_MAX_CONTENT_LEN;
constexpr size_t kRecordBytes = kRecordPayload + 1024;
TlsRecordPool<kRecordBytes, kRecordPayload> s_records;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
bool s_installed = false;
uint32_t s_hits = 0;
uint32_t s_spills = 0;

void* allocateTls(size_t count, size_t size) {
  if (count != 0 && size > std::numeric_limits<size_t>::max() / count) {
    return nullptr;
  }
  const size_t bytes = count * size;
  void* pointer = nullptr;
  if (s_records.accepts(bytes)) {
    portENTER_CRITICAL(&s_lock);
    pointer = s_records.allocate(bytes);
    if (pointer != nullptr) ++s_hits;
    else ++s_spills;
    portEXIT_CRITICAL(&s_lock);
  }
  // Preserve the SDK's allocation-capability and free-pairing policy.
  return pointer != nullptr ? pointer : esp_mbedtls_mem_calloc(count, size);
}

void freeTls(void* pointer) {
  portENTER_CRITICAL(&s_lock);
  const bool owned = s_records.release(pointer);
  portEXIT_CRITICAL(&s_lock);
  if (!owned) esp_mbedtls_mem_free(pointer);
}

}  // namespace

bool initTlsRecordPool() {
  if (!s_installed) {
    s_installed = mbedtls_platform_set_calloc_free(allocateTls, freeTls) == 0;
    Serial.printf("tls pool: installed=%d reserved=%u record_capacity=%u\n",
                  s_installed ? 1 : 0, static_cast<unsigned>(2 * kRecordBytes),
                  static_cast<unsigned>(kRecordBytes));
  }
  return s_installed;
}

bool tlsRecordPoolIdle() {
  portENTER_CRITICAL(&s_lock);
  const bool idle = s_installed && s_records.available() == 2;
  portEXIT_CRITICAL(&s_lock);
  return idle;
}

void printTlsRecordPoolDiagnostics() {
  portENTER_CRITICAL(&s_lock);
  const size_t available = s_records.available();
  const uint32_t hits = s_hits;
  const uint32_t spills = s_spills;
  portEXIT_CRITICAL(&s_lock);
  Serial.printf("tls pool: free=%u hits=%lu spills=%lu\n",
                static_cast<unsigned>(available), static_cast<unsigned long>(hits),
                static_cast<unsigned long>(spills));
}

}  // namespace services::net
