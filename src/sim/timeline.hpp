#pragma once
#include "sim/sim.hpp"
#include "sim/snapshot.hpp"
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace sim {

enum class TimelineErr {
  // later than now
  InFuture,
  // earlier than the oldest keyframe kept
  TooFarBack,
};

struct TimelineConfig {
  // World time between keyframes. Going back replays at most this much.
  TimeInt keyframe_interval = 1'000'000'000;
  // Keyframes kept. Taking a new one drops the oldest, so how far back you
  // can go is about keyframe_count * keyframe_interval behind the latest
  // time reached.
  uint32_t keyframe_count = 16;
};

// A Sim driven by a log of timestamped inputs: the record of what happened.
// The sim's state at any time follows from the log, so inputs can be added
// anywhere in time, even in the past, and the present is recomputed to
// match. Going back moves the present without forgetting anything: the
// inputs after it are still in the log and play again as time passes.
//
// Not part of the sim: rollback isn't an input, and none of this is sim
// state. Time here is world time since the Timeline was made.
//
// The present (and every time rollback_to goes to) includes every input
// logged at or before it. Inputs at the same time apply in the order they
// were logged.
class Timeline {
public:
  // Starts from an empty Sim at time 0.
  explicit Timeline(TimelineConfig config = {});
  // Starts from `start`'s state at time 0 (its own clock: the time the
  // snapshot was taken at doesn't carry over). Same inputs from there give
  // the same states the snapshot's own sim would have.
  Timeline(TimelineConfig config, const Snapshot &start);

  // An input now, then `dt` of time passing: insert(input, now()) followed
  // by advance(dt). Returns the input's result.
  SimResult exec(SimInput input, TimeInt dt);

  // Lets `dt` of world time pass, applying logged inputs as their times
  // come up.
  void advance(TimeInt dt);

  // Logs an input at world time `at`, after any already logged at `at`.
  // At or before now it takes effect right away: the sim is re-simulated
  // from there to now, adjusting everything after it. Later than now, it
  // applies when time gets there.
  std::expected<void, TimelineErr> insert(SimInput input, TimeInt at);

  // Moves the present to `t` (at most now): the sim as it was then. The log
  // is kept; advancing from here plays the later inputs again.
  std::expected<void, TimelineErr> rollback_to(TimeInt t);

  TimeInt now() const noexcept { return now_; }
  // the earliest time insert and rollback_to accept (the oldest keyframe)
  TimeInt earliest() const noexcept;

  const Sim &sim() const noexcept { return *sim_; }

private:
  struct Entry {
    TimeInt at;
    SimInput input;
  };

  // The state at index * keyframe_interval, before any inputs at that time.
  struct Keyframe {
    uint64_t index = 0;
    bool valid = false;
    Snapshot snap;
  };

  TimelineConfig config;
  std::unique_ptr<Sim> sim_;
  TimeInt now_ = 0;

  // Sorted by time; equal times in the order logged. Entries older than the
  // oldest keyframe are dropped (nothing can replay from before it).
  std::vector<Entry> log;
  // The first entry later than now_: everything before it is applied.
  size_t next = 0;

  // Keyframe `index` lives in slot index % keyframe_count, so taking a new
  // one overwrites the one keyframe_count intervals older.
  std::vector<Keyframe> keyframes;

  // Advances to `target`, applying entries and taking keyframes on the way.
  void run_to(TimeInt target);
  // Puts the sim at time `t` from the newest valid keyframe at or before it.
  void resume_at(TimeInt t);
  void take_keyframe_if_missing(uint64_t index);
  void drop_unreplayable_entries();
};

} // namespace sim
