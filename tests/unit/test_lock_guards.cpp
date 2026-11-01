// gitmount unit tests — exception-safety lock guards (issue #4, RFC 0000 §3.4).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "lock_guards.hpp"

using gitmount::GeneratorFinish;
using gitmount::ScopedUnlock;
using gitmount::SingleFlight;

namespace {

// try_lock from a fresh thread: true exactly when m is unlocked (avoids the
// same-thread self-try ambiguity of a default mutex).
bool try_lock_from_thread(std::mutex& m) {
  bool got = false;
  std::thread probe([&] {
    if (m.try_lock()) {
      got = true;
      m.unlock();
    }
  });
  probe.join();
  return got;
}

// Polls flag for up to ~10s; a wedged guard then fails the test instead of
// hanging the suite (the worker threads are detached, so a failure can still
// exit the process cleanly).
bool settled_within_deadline(const std::atomic<bool>& flag) {
  for (int i = 0; i < 1000; ++i) {
    if (flag.load()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return flag.load();
}

}  // namespace

// ---------------------------------------------------------------------------
// ScopedUnlock
// ---------------------------------------------------------------------------

TEST_CASE("ScopedUnlock leaves the mutex unlocked while alive and relocks at scope exit",
          "[guards]") {
  std::mutex m;
  m.lock();
  {
    ScopedUnlock u(m);
    CHECK(try_lock_from_thread(m));  // window: another thread can take it
  }
  CHECK_FALSE(try_lock_from_thread(m));  // scope end: locked again
  m.unlock();                            // restore for destruction
}

TEST_CASE("ScopedUnlock relocks during exception unwinding; the outer lock_guard stays safe",
          "[guards]") {
  std::mutex m;
  bool caught = false;
  try {
    std::lock_guard<std::mutex> lk(m);  // the production pattern (cb_*)
    {
      ScopedUnlock u(m);
      throw std::runtime_error("allocation failed inside the window");
    }
  } catch (const std::runtime_error&) {
    caught = true;
    // ~ScopedUnlock ran before ~lock_guard, so the guard destructed a LOCKED
    // mutex (no unlock-of-unlocked UB). The mutex itself must now be in a
    // normally usable state: no deadlock, no double-unlock corruption.
    m.lock();
    m.unlock();
    CHECK(try_lock_from_thread(m));
    m.lock();
    m.unlock();
  }
  REQUIRE(caught);
}

// ---------------------------------------------------------------------------
// SingleFlight
// ---------------------------------------------------------------------------
// The rider threads mirror open_commits' rider path: enter while a generation
// is in flight, wait for the epoch to change, then share the published
// outcome. Handshake: `parked` is set while the rider holds the flight mutex,
// so once the generator (re)acquires it the rider is parked on the condvar.

TEST_CASE("SingleFlight: a throwing generator resets the flight and wakes riders in time",
          "[guards]") {
  SingleFlight sf;
  std::atomic<bool> in_flight{false};
  std::atomic<bool> parked{false};
  std::atomic<bool> generation_ok{true};  // the shared outcome
  std::atomic<bool> rider_returned{false};
  std::atomic<bool> rider_saw_failure{false};
  std::atomic<bool> rider_finished{false};

  std::thread rider([&] {
    while (!in_flight.load()) std::this_thread::yield();
    {
      auto g = sf.hold();
      parked.store(true);  // mutex held: the generator cannot finish yet
      const bool generator = sf.begin_or_wait(g);
      const bool ok = generation_ok.load();  // share the outcome…
      rider_saw_failure = !ok;
      rider_returned = !generator;  // …then report (ordering is load-bearing)
    }
    rider_finished = true;
  });
  rider.detach();

  auto g = sf.hold();
  REQUIRE(sf.begin_or_wait(g));  // this thread is the generator
  bool threw = false;
  try {
    GeneratorFinish fin(sf, g);
    g.unlock();
    in_flight.store(true);
    while (!parked.load()) std::this_thread::yield();
    generation_ok.store(false);  // the failure is settled before the flight ends
    throw std::runtime_error("bad_alloc during generation");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw);
  REQUIRE(g.owns_lock());  // ~GeneratorFinish re-locked through the same unique_lock

  g.unlock();  // let the rider re-acquire inside the condvar wait and wake
  REQUIRE(settled_within_deadline(rider_returned));  // rider woke: no wedge
  REQUIRE(rider_returned.load());                    // …as a rider, not a generator
  REQUIRE(rider_saw_failure.load());                 // …and saw the failed outcome
  REQUIRE(settled_within_deadline(rider_finished));  // rider fully done with sf

  // Flag reset: the next begin_or_wait immediately becomes the generator
  // (open_commits' "next open retries" semantics, §3.5).
  g.lock();
  REQUIRE(sf.begin_or_wait(g));
  sf.finish(g);
  g.unlock();
}

TEST_CASE("SingleFlight: a successful generation shares its outcome with the rider", "[guards]") {
  SingleFlight sf;
  std::atomic<bool> in_flight{false};
  std::atomic<bool> parked{false};
  std::atomic<bool> outcome_ok{false};  // stands in for commits_buffer_
  std::atomic<bool> rider_returned{false};
  std::atomic<bool> rider_saw_success{false};
  std::atomic<bool> rider_finished{false};

  std::thread rider([&] {
    while (!in_flight.load()) std::this_thread::yield();
    {
      auto g = sf.hold();
      parked.store(true);
      const bool generator = sf.begin_or_wait(g);
      const bool ok = outcome_ok.load();
      rider_saw_success = ok;
      rider_returned = !generator;
    }
    rider_finished = true;
  });
  rider.detach();

  auto g = sf.hold();
  REQUIRE(sf.begin_or_wait(g));  // generator
  {
    GeneratorFinish fin(sf, g);
    g.unlock();
    in_flight.store(true);
    while (!parked.load()) std::this_thread::yield();
    // g.lock() can only return once the rider has parked on the condvar.
    g.lock();
    outcome_ok.store(true);  // publish while still in flight
    fin.finish();            // normal-path manual finish (done_ set; dtor no-ops)
  }
  g.unlock();

  REQUIRE(settled_within_deadline(rider_returned));
  REQUIRE(rider_returned.load());
  REQUIRE(rider_saw_success.load());
  REQUIRE(settled_within_deadline(rider_finished));

  // Ready for the next generation right away.
  auto g2 = sf.hold();
  REQUIRE(sf.begin_or_wait(g2));
  sf.finish(g2);
}

TEST_CASE("SingleFlight: finish is idempotent-safe and GeneratorFinish fires exactly once",
          "[guards]") {
  SingleFlight sf;
  auto g = sf.hold();
  REQUIRE(sf.begin_or_wait(g));
  const std::uint64_t before = sf.epoch(g);
  {
    GeneratorFinish fin(sf, g);
    fin.finish();  // manual
  }  // ~GeneratorFinish: already done — must not finish again
  CHECK(sf.epoch(g) == before + 1);
  CHECK(sf.begin_or_wait(g));  // flag reset, next generation can start

  // A bare double finish is not UB: it only bumps the epoch again and
  // re-notifies (no riders are parked in this scenario).
  sf.finish(g);
  sf.finish(g);
  CHECK(sf.epoch(g) == before + 3);
  CHECK(sf.begin_or_wait(g));
  sf.finish(g);
}
