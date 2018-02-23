#include "reactor.hh"

namespace seastar::alien {

class pollfn final : public seastar::reactor::pollfn {
  reactor& _r;
public:
  pollfn(reactor& r) : _r(r) {}
  // Return true if work was done (false = idle)
  bool poll() override;
  // Checks if work needs to be done, but without actually doing any
  // returns true if works needs to be done (false = idle)
  virtual bool pure_poll() override;
  // Tries to enter interrupt mode.
  //
  // If it returns true, then events from this poller will wake
  // a sleeping idle loop, and exit_interrupt_mode() must be called
  // to return to normal polling.
  //
  // If it returns false, the sleeping idle loop may not be entered.
  virtual bool try_enter_interrupt_mode() override;
  virtual void exit_interrupt_mode() override;
};

} // namespace seastar::alien
