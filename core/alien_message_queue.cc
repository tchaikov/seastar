#include "alien_message_queue.hh"
#include "metrics.hh"
#include "prefetch.hh"

namespace seastar::alien {

message_queue::message_queue(reactor *to)
  : _pending(to)
{}

void message_queue::stop() {
  _metrics.clear();
}

void message_queue::move_pending() {
  auto begin = pending_fifo().cbegin();
  auto end = pending_fifo().cend();
  size_t nr = 0;
  auto item = begin;
  while (item != end) {
    if (!_pending.bounded_push(*item++)) {
      break;
    }
    nr++;
  }
  if (!nr) {
    return;
  }
  pending_fifo().erase(begin, item);
  _current_queue_length += nr;
  _last_snt_batch = nr;
  _sent += nr;
}

void message_queue::submit_item(std::unique_ptr<message_queue::work_item> item) {
  pending_fifo().push_back(item.get());
  item.release();
  if (pending_fifo().size() >= batch_size) {
    move_pending();
  }
}

void message_queue::flush_request_batch() {
  if (!pending_fifo().empty()) {
    move_pending();
  }
}

bool message_queue::pure_poll_rx() const {
  return !_pending.empty();
}

template<size_t PrefetchCnt, typename Func>
size_t message_queue::process_queue(lf_queue& q, Func process) {
  // copy batch to local memory in order to minimize
  // time in which cross-cpu data is accessed
  work_item* items[queue_length + PrefetchCnt];
  work_item* wi;
  if (!q.pop(wi))
    return 0;
  // start prefecthing first item before popping the rest to overlap memory
  // access with potential cache miss the second pop may cause
  prefetch<2>(wi);
  size_t nr = 0;
  while (q.pop(items[nr])) {
    ++nr;
  }
  std::fill(std::begin(items) + nr, std::begin(items) + nr + PrefetchCnt, nr ? items[nr - 1] : wi);
  unsigned i = 0;
  do {
    prefetch_n<2>(std::begin(items) + i, std::begin(items) + i + PrefetchCnt);
    process(wi);
    wi = items[i++];
  } while(i <= nr);

  return nr + 1;
}

size_t message_queue::process_incoming() {
  auto nr = process_queue<prefetch_cnt>(_pending, [this] (work_item* wi) {
      wi->process();
      delete wi;
    });
  _received += nr;
  _last_rcv_batch = nr;
  _current_queue_length -= nr;
  return nr;
}

void message_queue::start() {
  _tx.init();
  namespace sm = seastar::metrics;
  char instance[10];
  std::snprintf(instance, sizeof(instance), "%u", engine().cpu_id());
  _metrics.add_group("alien", {
      // queue_length     value:GAUGE:0:U
      // Absolute value of num packets in last tx batch.
      sm::make_queue_length("send_batch_queue_length", _last_snt_batch, sm::description("Current send batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
      sm::make_queue_length("receive_batch_queue_length", _last_rcv_batch, sm::description("Current receive batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
      sm::make_queue_length("send_queue_length", _current_queue_length, sm::description("Current send queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
      // total_operations value:DERIVE:0:U
      sm::make_derive("total_received_messages", _received, sm::description("Total number of received messages"), {sm::shard_label(instance)})(sm::metric_disabled),
      // total_operations value:DERIVE:0:U
      sm::make_derive("total_sent_messages", _sent, sm::description("Total number of sent messages"), {sm::shard_label(instance)})(sm::metric_disabled),
  });
}

message_queue* smp::_qs = nullptr;

} // namespace seastar::alien
