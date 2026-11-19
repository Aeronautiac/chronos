#include "timeline.hpp"
#include <algorithm>
#include <cassert>
#include <limits>

namespace sim {

namespace {

constexpr TimeInt NEVER = std::numeric_limits<TimeInt>::max();

} // namespace

Timeline::Timeline(TimelineConfig config_)
    : config(config_), sim_(std::make_unique<Sim>()) {
  assert(config.keyframe_count > 0 && config.keyframe_interval > 0);
  keyframes.resize(config.keyframe_count);
  take_keyframe_if_missing(0);
}

Timeline::Timeline(TimelineConfig config_, const Snapshot &start)
    : config(config_), sim_(std::make_unique<Sim>()) {
  assert(config.keyframe_count > 0 && config.keyframe_interval > 0);
  keyframes.resize(config.keyframe_count);
  sim_->restore(start);
  take_keyframe_if_missing(0);
}

TimeInt Timeline::earliest() const noexcept {
  TimeInt oldest = NEVER;
  for (const Keyframe &kf : keyframes) {
    if (kf.valid) {
      oldest = std::min(oldest, kf.index * config.keyframe_interval);
    }
  }
  assert(oldest != NEVER);
  return oldest;
}

void Timeline::take_keyframe_if_missing(uint64_t index) {
  Keyframe &slot = keyframes[index % config.keyframe_count];
  if (slot.valid && slot.index == index) {
    // still matches the log (inserting before it would have invalidated it)
    return;
  }
  assert(now_ == index * config.keyframe_interval);
  slot = Keyframe{.index = index, .valid = true, .snap = sim_->save()};
  drop_unreplayable_entries();
}

void Timeline::drop_unreplayable_entries() {
  // replays only ever start at a keyframe, so nothing before the oldest one
  // can be needed again
  const TimeInt oldest = earliest();
  const auto keep = std::ranges::lower_bound(log, oldest, {}, &Entry::at);
  const auto dropped = static_cast<size_t>(keep - log.begin());
  assert(dropped <= next);
  log.erase(log.begin(), keep);
  next -= dropped;
}

void Timeline::run_to(TimeInt target) {
  assert(target >= now_);
  while (now_ < target) {
    const TimeInt next_keyframe =
        (now_ / config.keyframe_interval + 1) * config.keyframe_interval;
    const TimeInt next_input = next < log.size() ? log[next].at : NEVER;
    const TimeInt stop = std::min({next_keyframe, next_input, target});

    // splitting time up like this runs the same ticks as one long advance
    sim_->advance(stop - now_);
    now_ = stop;

    // arrived at `stop`: keyframe first (before its inputs), then its inputs
    if (now_ % config.keyframe_interval == 0) {
      take_keyframe_if_missing(now_ / config.keyframe_interval);
    }
    while (next < log.size() && log[next].at == now_) {
      (void)sim_->apply(log[next].input);
      next += 1;
    }
  }
}

void Timeline::resume_at(TimeInt t) {
  // the newest valid keyframe at or before t
  const uint64_t limit = t / config.keyframe_interval;
  const Keyframe *best = nullptr;
  for (const Keyframe &kf : keyframes) {
    if (kf.valid && kf.index <= limit && (!best || kf.index > best->index)) {
      best = &kf;
    }
  }
  assert(best && "callers check t >= earliest()");

  sim_->restore(best->snap);
  now_ = best->index * config.keyframe_interval;
  // the keyframe is from before the inputs at its own time
  next = static_cast<size_t>(
      std::ranges::lower_bound(log, now_, {}, &Entry::at) - log.begin());
  while (next < log.size() && log[next].at == now_) {
    (void)sim_->apply(log[next].input);
    next += 1;
  }
  run_to(t);
}

SimResult Timeline::exec(SimInput input, TimeInt dt) {
  // After a rollback, keyframes from later than now are still around, and
  // this input changes everything after now: they don't match any more.
  // (One exactly at now does: it's from before the inputs at its time.)
  for (Keyframe &kf : keyframes) {
    if (kf.valid && kf.index * config.keyframe_interval > now_) {
      kf.valid = false;
    }
  }
  // after everything already at now, before anything later
  log.insert(log.begin() + static_cast<std::ptrdiff_t>(next),
             Entry{.at = now_, .input = input});
  next += 1;
  const SimResult res = sim_->apply(input);
  advance(dt);
  return res;
}

void Timeline::advance(TimeInt dt) { run_to(now_ + dt); }

std::expected<void, TimelineErr> Timeline::insert(SimInput input,
                                                  TimeInt at) {
  if (at < earliest()) {
    return std::unexpected(TimelineErr::TooFarBack);
  }
  if (at == now_) {
    (void)exec(input, 0);
    return {};
  }

  // after everything already logged at `at`
  const auto pos = std::ranges::upper_bound(log, at, {}, &Entry::at);
  log.insert(pos, Entry{.at = at, .input = input});
  if (at > now_) {
    // applies when time gets there (it went in after `next`, which is the
    // first entry later than now, so `next` still is)
    return {};
  }

  // In the past: keyframes after `at` no longer match the log (one exactly
  // at `at` does: it's from before the inputs at its time). Re-simulate
  // from the last good one back to now.
  for (Keyframe &kf : keyframes) {
    if (kf.valid && kf.index * config.keyframe_interval > at) {
      kf.valid = false;
    }
  }
  resume_at(now_);
  return {};
}

std::expected<void, TimelineErr> Timeline::rollback_to(TimeInt t) {
  if (t > now_) {
    return std::unexpected(TimelineErr::InFuture);
  }
  if (t < earliest()) {
    return std::unexpected(TimelineErr::TooFarBack);
  }
  resume_at(t);
  return {};
}

} // namespace sim
