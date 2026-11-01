// gitmount — read-only git-to-FUSE filesystem (RFC 0000).
// SPDX-FileCopyrightText: 2026 The gitmount authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Exception-safety lock guards (RFC 0000 §3.4 concurrency model).
//
// ScopedUnlock replaces the hand-written `mu_.unlock() … mu_.lock()` windows
// of the segmented-loading path: the destructor re-locks during exception
// unwinding BEFORE the surrounding std::lock_guard runs, so the outer guard
// always destructs a locked mutex — an allocation failure inside a window
// (repository-content-sized, e.g. std::bad_alloc from an uncapped object
// read) can no longer turn into unlock-of-unlocked-mutex UB.
//
// SingleFlight owns the /commits first-open generation state machine
// (RFC 0000 §3.4/§3.5): whichever way the generator exits — including an
// exception — the in-flight flag is reset, the epoch advances and riders are
// woken, so a failed generation maps to EIO-and-retry instead of parking
// every later open on the condition variable (and, transitively, exhausting
// the libfuse worker pool).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace gitmount {

// Unlocks the given mutex on construction, re-locks it on destruction (the
// normal scope-exit path and the exception-unwinding path alike). Not
// copyable. The mutex must outlive the guard and be locked at construction.
class ScopedUnlock {
 public:
  explicit ScopedUnlock(std::mutex& m) : m_(m) { m_.unlock(); }

  ScopedUnlock(const ScopedUnlock&) = delete;
  ScopedUnlock& operator=(const ScopedUnlock&) = delete;

  ~ScopedUnlock() { m_.lock(); }

 private:
  std::mutex& m_;
};

// Single-flight state for one idempotent-on-failure generation ("commits"):
// a mutex/condvar pair plus an in-flight flag and a wake-up epoch. The
// caller-visible pattern (see open_commits):
//
//   auto g = flight.hold();
//   if (flight.begin_or_wait(g)) {        // generator (flag now set)
//     GeneratorFinish fin(flight, g);     // safety net on every exit path
//     g.unlock();
//     … generate (may throw / fail) …
//     g.lock();
//     … publish result …
//     fin.finish();                       // or let ~GeneratorFinish do it
//   } else {                              // rider: generation finished
//     … inspect the shared outcome …
//   }
//
// Riders wait for the epoch to CHANGE, not for a "success" value: they share
// whatever outcome the generation left behind (a published buffer, or the
// absence of one = failure). finish() bumps the epoch exactly once per
// generation, so each generation's riders wake exactly once.
//
// Lock ordering: the internal mutex is the OUTERMOST lock of the /commits
// path (it never nests inside the filesystem-wide mutex being held while it
// is acquired, only the reverse) — callers must preserve that order.
class SingleFlight {
 public:
  SingleFlight() = default;
  SingleFlight(const SingleFlight&) = delete;
  SingleFlight& operator=(const SingleFlight&) = delete;

  // Returns a lock on the internal mutex; required by begin_or_wait/finish.
  std::unique_lock<std::mutex> hold() { return std::unique_lock<std::mutex>(mu_); }

  // g must lock this->mu_ (via hold()) and be held. When no generation is
  // in flight: marks one in flight and returns true (caller generates).
  // Otherwise waits (releasing g) until the in-flight generation finishes
  // and returns false (caller rides along and inspects the shared outcome).
  bool begin_or_wait(std::unique_lock<std::mutex>& g) {
    if (!generating_) {
      generating_ = true;
      return true;
    }
    const std::uint64_t observed = epoch_;
    cv_.wait(g, [&] { return epoch_ != observed; });
    return false;
  }

  // Ends the in-flight generation: flag reset, epoch advanced, riders woken.
  // g must lock this->mu_ and be held. Idempotent-safe (a second call only
  // bumps the epoch and re-notifies) but each generation must finish exactly
  // once — GeneratorFinish's done flag is the single-shot guarantee.
  void finish(std::unique_lock<std::mutex>&) {
    generating_ = false;
    ++epoch_;
    cv_.notify_all();
  }

  // Current epoch; g must lock this->mu_ and be held. Diagnostics/tests.
  std::uint64_t epoch(std::unique_lock<std::mutex>&) const { return epoch_; }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool generating_ = false;  // guarded by mu_
  std::uint64_t epoch_ = 0;  // guarded by mu_; bumped once per generation
};

// RAII counterpart of SingleFlight::finish for the generator thread: on
// destruction (any exit path, including exception unwinding) ends the
// generation — re-locking through the SAME unique_lock when it was left
// unlocked for the generation itself (locking the raw mutex here would
// self-deadlock the normal path, where g is already held again). finish()
// marks the generation ended so a manual call followed by the destructor
// finishes exactly once.
class GeneratorFinish {
 public:
  GeneratorFinish(SingleFlight& flight, std::unique_lock<std::mutex>& g) : flight_(flight), g_(g) {}

  GeneratorFinish(const GeneratorFinish&) = delete;
  GeneratorFinish& operator=(const GeneratorFinish&) = delete;

  ~GeneratorFinish() { finish(); }

  void finish() {
    if (done_) return;
    if (!g_.owns_lock()) g_.lock();
    flight_.finish(g_);
    done_ = true;
  }

 private:
  SingleFlight& flight_;
  std::unique_lock<std::mutex>& g_;
  bool done_ = false;
};

}  // namespace gitmount
