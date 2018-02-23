#include "alien_message_queue.hh"
#include "alien_poller.hh"
#include "systemwide_memory_barrier.hh"

namespace seastar::alien {

bool pollfn::poll() {
  auto& queue = smp::_qs[engine().cpu_id()];
  return queue.process_incoming() != 0;
}

bool pollfn::pure_poll() {
  auto& queue = smp::_qs[engine().cpu_id()];
  queue.flush_request_batch();
  return queue.pure_poll_rx();
}

// the {enter/exit}_interrupt_mode() implementation are stolen from
// reactor::smp_pollfn::try_enter_interrupt_mode().
bool pollfn::try_enter_interrupt_mode() {
  // systemwide_memory_barrier() is very slow if run concurrently,
  // so don't go to sleep if it is running now.
  _r._sleeping.store(true, std::memory_order_relaxed);
  bool barrier_done = try_systemwide_memory_barrier();
  if (!barrier_done) {
    _r._sleeping.store(false, std::memory_order_relaxed);
    return false;
  }
  if (poll()) {
    // raced
    _r._sleeping.store(false, std::memory_order_relaxed);
    return false;
  }
  return true;
}

void pollfn::exit_interrupt_mode() {
  _r._sleeping.store(false, std::memory_order_relaxed);
}

} // namespace seastar::alien
