#pragma once

#include <chrono>

#include "envoy/event/timer.h"

#include "common/event/event_impl_base.h"
#include "common/event/libevent.h"

namespace Envoy {
namespace Event {

/**
 * libevent implementation of Timer.
 */
class TimerImpl : public Timer, ImplBase {
public:
  TimerImpl(Libevent::BasePtr& libevent, TimerCb cb);

  // Timer
  void disableTimer() override;
  void enableTimerInUs(const std::chrono::microseconds& d) override;
  bool enabled() override;

private:
  TimerCb cb_;
};

} // namespace Event
} // namespace Envoy
