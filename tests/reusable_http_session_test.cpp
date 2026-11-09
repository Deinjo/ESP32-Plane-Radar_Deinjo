#include "services/reusable_http_session.h"
#include "services/tls_slot.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {
struct Client {
  static inline unsigned created = 0;
  static inline std::vector<unsigned> events;
  unsigned id = ++created;
  bool online = false;
  bool alive = true;
  bool poisoned = false;
  Client() { events.push_back(10 + id); }
  ~Client() { alive = false; events.push_back(30 + id); }
  bool connected() const { return online; }
};

struct Http {
  Client* attached = nullptr;
  static inline unsigned created = 0;
  unsigned id = ++created;
  Http() { Client::events.push_back(20 + id); }
  ~Http() {
    // A real HTTPClient destructor may call stop() on its attached client.
    assert(attached == nullptr || attached->alive);
    Client::events.push_back(40 + id);
  }
};
}

// Current function: prepare(). Actual production owner, fake HTTP/TLS seam.
// Structural regression, not evidence that a real endpoint is available.
int main() {
  using Session = services::net::ReusableHttpSession<Client, Http>;
  {
    Session session;
    assert(session.prepare());
    session.http().attached = &session.client();
    session.client().online = true;
    // More than the incident's 1,000 responses must not churn a healthy TLS session.
    for (unsigned i = 0; i < 1500; ++i) assert(!session.prepare());
    assert(Client::created == 1 && Http::created == 1);

    session.client().online = false; // peer closes keep-alive
    session.client().poisoned = true;
    assert(session.prepare());
    assert(!session.client().poisoned && session.client().id == 2);
    assert(Http::created == 2);
    assert((Client::events == std::vector<unsigned>{11,21,41,31,12,22}));

    session.http().attached = &session.client();
    session.client().online = true;
    session.discard(); // request/JSON failure: release immediately, retry later
    assert(Client::events.back() == 32);
    assert(Client::created == 2); // no new allocation during backoff
    session.discard(); // safe if cleanup is requested twice
    assert(session.prepare());
    assert(session.client().id == 3 && Http::created == 3);
    session.http().attached = &session.client();
  }
  assert(Client::events[Client::events.size()-2] == 43);
  assert(Client::events.back() == 33);
  using services::net::tlsSlotHasReserve;
  assert(tlsSlotHasReserve(48000, 12000, 12000, 48000, 12000));
  assert(tlsSlotHasReserve(53520, 20468, 20468, 48000, 12000));
  assert(!tlsSlotHasReserve(47999, 32000, 32000, 48000, 12000));
  assert(!tlsSlotHasReserve(60000, 11999, 32000, 48000, 12000));
  assert(!tlsSlotHasReserve(60000, 32000, 11999, 48000, 12000));
  assert(!tlsSlotHasReserve(30000, 9204, 9204, 48000, 12000));
  std::puts("PASS: healthy session reuse beyond 1000 calls, peer-close replacement,");
  std::puts("      failed-session release, deferred retry allocation, HTTP-before-TLS destruction");
  std::puts("      TLS slot total/contiguous/internal reserve boundary checks");
}
