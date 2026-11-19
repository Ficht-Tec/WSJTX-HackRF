#include "tx_iq_queue.hpp"
#include <algorithm>
#include <cstring>

bool TxIqQueue::push(const std::vector<std::int8_t>& iq) {
  if(iq.empty()) return true;
  if(iq.size()%2) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if(producer_finished_) return false;
  if(iq.size()>kMaxBytes-queued_) {++overflow_count_;return false;}
  if(!first_push_seen_) { first_push_=std::chrono::steady_clock::now(); first_push_seen_=true; }
  ++push_count_;
  blocks_.push_back(iq);
  queued_+=iq.size();
  peak_=std::max(peak_,queued_);
  ready_cv_.notify_all();
  return true;
}
bool TxIqQueue::finish(const std::vector<std::int8_t>& final_iq) {
  if(final_iq.size()%2) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if(producer_finished_) return false;
  if(final_iq.size()>kMaxBytes-queued_) {++overflow_count_;return false;}
  if(!final_iq.empty()) {
    if(!first_push_seen_) {first_push_=std::chrono::steady_clock::now();first_push_seen_=true;}
    ++push_count_;
    blocks_.push_back(final_iq);
    queued_+=final_iq.size();
    peak_=std::max(peak_,queued_);
  }
  // The USB callback sees the final block and the end-of-production marker
  // in the same critical section. No padding is misclassified at this boundary.
  producer_finished_=true;
  ready_cv_.notify_all();
  return true;
}
bool TxIqQueue::wait_for_bytes(std::size_t minimum, std::chrono::milliseconds timeout) {
  if(minimum>kMaxBytes) return false;
  std::unique_lock<std::mutex> lock(mutex_);
  return ready_cv_.wait_for(lock,timeout,[this,minimum]{return queued_>=minimum;});
}

void TxIqQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  blocks_.clear();queued_=0;offset_=0;
  underrun_bytes_=0;sent_bytes_=0;overflow_count_=0;
  active_underrun_bytes_=0;closing_padding_bytes_=0;
  producer_finished_=false;active_underrun_events_=0;closing_padding_events_=0;
  peak_=0;callbacks_=0;callback_clock_started_=false;
  first_push_seen_=false;push_count_=0;
  underrun_events_total_=0;last_underrun_ms_=-1;underrun_event_log_.clear();
}
int TxIqQueue::callback(hackrf_transfer* t) {
  auto* q=static_cast<TxIqQueue*>(t?t->tx_ctx:nullptr);
  if(!q || !t->buffer || t->buffer_length<=0) return -1;
  const std::size_t cap=static_cast<std::size_t>(t->buffer_length);
  std::size_t done=0;
  bool closing_padding=false;
  {
    std::lock_guard<std::mutex> lock(q->mutex_);
    ++q->callbacks_;
    if(!q->callback_clock_started_) {
      q->first_callback_=std::chrono::steady_clock::now();
      q->callback_clock_started_=true;
    }
    while(done<cap && !q->blocks_.empty()) {
      const auto& front=q->blocks_.front();
      const std::size_t n=std::min(cap-done,front.size()-q->offset_);
      std::memcpy(t->buffer+done,front.data()+q->offset_,n);
      done+=n;q->offset_+=n;q->queued_-=n;
      if(q->offset_==front.size()) {q->blocks_.pop_front();q->offset_=0;}
    }
    closing_padding=q->producer_finished_;
  }
  if(done<cap) {
    // Incomplete callback: record the instant and queue occupancy AT the event,
    // not the later once-per-second snapshot. Never log from the USB thread.
    const auto now=std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(q->mutex_);
      const double ms=std::chrono::duration<double,std::milli>(now-q->first_callback_).count();
      ++q->underrun_events_total_;
      if(closing_padding) ++q->closing_padding_events_;
      else ++q->active_underrun_events_;
      q->last_underrun_ms_=ms;
      if(q->underrun_event_log_.size()<kMaxLoggedEvents) {
        UnderrunEvent event;
        event.from_first_callback_ms=ms;
        event.callback_number=q->callbacks_;
        event.missing_bytes=cap-done;
        event.queued_before=q->queued_;
        event.closing_padding=closing_padding;
        q->underrun_event_log_.push_back(event);
      }
    }
    std::memset(t->buffer+done,0,cap-done);
    q->underrun_bytes_+=cap-done;
    if(closing_padding) q->closing_padding_bytes_+=cap-done;
    else q->active_underrun_bytes_+=cap-done;
  }
  q->sent_bytes_+=done;
  t->valid_length=t->buffer_length;
  return 0;
}

TxIqQueue::Snapshot TxIqQueue::snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  const double first_push_relative=(first_push_seen_ && callback_clock_started_)
    ? std::chrono::duration<double,std::milli>(first_push_-first_callback_).count() : -999999.0;
  return {queued_,peak_,callbacks_,underrun_bytes_.load(),sent_bytes_.load(),overflow_count_.load(),underrun_events_total_,active_underrun_bytes_.load(),active_underrun_events_,closing_padding_bytes_.load(),closing_padding_events_,producer_finished_,last_underrun_ms_,first_push_relative,push_count_};
}

std::vector<TxIqQueue::UnderrunEvent> TxIqQueue::underrun_events() {
  std::lock_guard<std::mutex> lock(mutex_);
  return underrun_event_log_;
}
