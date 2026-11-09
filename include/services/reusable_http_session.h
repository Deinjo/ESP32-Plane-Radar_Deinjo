#pragma once

#include <new>

namespace services::net {

// Reuse a healthy transport, not a stopped/failed TLS object's internal state.
// HTTP must be destroyed before the client it references. Storage is inline;
// only the concrete clients' own normal session allocations use the heap.
template <typename Client, typename Http>
class ReusableHttpSession {
 public:
  bool prepare() {
    if (client_ && client_->connected()) {
      return false;
    }
    discard();
    client_ = new (static_cast<void*>(client_storage_)) Client();
    http_ = new (static_cast<void*>(http_storage_)) Http();
    return true;
  }

  void discard() {
    if (http_ != nullptr) {
      http_->~Http();
      http_ = nullptr;
    }
    if (client_ != nullptr) {
      client_->~Client();
      client_ = nullptr;
    }
  }

  Client& client() { return *client_; }
  Http& http() { return *http_; }

  ReusableHttpSession() = default;
  ~ReusableHttpSession() { discard(); }
  ReusableHttpSession(const ReusableHttpSession&) = delete;
  ReusableHttpSession& operator=(const ReusableHttpSession&) = delete;

 private:
  // The Arduino framework's effective language mode is C++11; do not require
  // std::optional or change every dependency's build flags for this owner.
  alignas(Client) unsigned char client_storage_[sizeof(Client)];
  alignas(Http) unsigned char http_storage_[sizeof(Http)];
  Client* client_ = nullptr;
  Http* http_ = nullptr;
};

}  // namespace services::net
