#ifndef ALIEN_MESSAGE_QUEUE_HH_
#define ALIEN_MESSAGE_QUEUE_HH_

#include <deque>
#include <memory>

#include <boost/lockfree/queue.hpp>

#include "cacheline.hh"
#include "sstring.hh"
#include "metrics_registration.hh"
#include "reactor.hh"


namespace seastar::alien {

class pollfn;

class message_queue {
  static constexpr size_t queue_length = 128;
  static constexpr size_t batch_size = 16;
  static constexpr size_t prefetch_cnt = 2;
  struct work_item;
  struct lf_queue_remote {
    reactor* remote;
  };
  using lf_queue_base = boost::lockfree::queue<work_item*,
                          boost::lockfree::capacity<queue_length>>;
  // use inheritence to control placement order
  struct lf_queue : lf_queue_remote, lf_queue_base {
    lf_queue(reactor* remote) : lf_queue_remote{remote} {}
  } _pending;
  struct alignas(seastar::cache_line_size) {
    size_t _sent = 0;
    size_t _last_snt_batch = 0;
    size_t _current_queue_length = 0;
  };
  // keep this between two structures with statistics
  // this makes sure that they have at least one cache line
  // between them, so hw prefecther will not accidentally prefetch
  // cache line used by aother cpu.
  metrics::metric_groups _metrics;
  struct alignas(seastar::cache_line_size) {
    size_t _received = 0;
    size_t _last_rcv_batch = 0;
  };
  struct work_item {
    virtual ~work_item() = default;
    virtual void process() = 0;
  };
  template <typename  Func>
  struct async_work_item : work_item {
    Func _func;
    async_work_item(Func&& func) : _func(std::move(func)) {}
    void process() override {
      _func();
    }
  };
  union tx_side {
    tx_side() {};
    ~tx_side() {};
    // initialize pending_fifo on sending cpu
    void init() { new (&a) aa; }
    struct aa {
      std::deque<work_item*> pending_fifo;
    } a;
  } _tx;

public:
  message_queue(reactor *to);
  template <typename Func>
  void submit(Func&& func) {
    auto wi = std::make_unique<async_work_item<Func>>(std::forward<Func>(func));
    submit_item(std::move(wi));
  }
  void start();
  void stop();
  template<size_t PrefetchCnt, typename Func>
  size_t process_queue(lf_queue& q, Func process);
  size_t process_incoming();
private:
  inline std::deque<work_item*>& pending_fifo() {
    return _tx.a.pending_fifo;
  }
  void submit_item(std::unique_ptr<work_item> wi);
  void move_pending();
  void flush_request_batch();
  bool pure_poll_rx() const;
  friend pollfn;
};

struct smp {
  static message_queue *_qs;
};

/// Runs a function on a remote core from an alien thread where engine() is not available.
///
/// \param t designates the core to run the function on
/// \param func a callable to run on core \c t.  If \c func is a temporary object,
///          its lifetime will be extended by moving it.  If @func is a reference,
///          the caller must guarantee that it will survive the call.
/// \note the func should return \c void. as we cannot identify the alien thread,
///          hence we are not able to post the fulfilled promise to the message
///          queue managed by the core executing the alien thread which is
///          interested to the return value.
template <typename Func>
void submit_to(unsigned t, Func&& func) {
  smp::_qs[t].submit(std::forward<Func>(func));
}

} // namespace seastar::alien

#endif  // ALIEN_MESSAGE_QUEUE_HH_
