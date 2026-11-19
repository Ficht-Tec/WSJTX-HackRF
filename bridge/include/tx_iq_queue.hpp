#pragma once
#include <libhackrf/hackrf.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

// USB callback consumes a bounded FIFO of interleaved signed IQ bytes.
// Owned by main thread, fed by TxSimCycleWorker. No file reads or disk dependency.
class TxIqQueue {
public:
  bool push(const std::vector<std::int8_t>& iq);
  // Append final IQ and mark producer complete ATOMICALLY with respect to USB callback.
  bool finish(const std::vector<std::int8_t>& final_iq);
  void clear();
  // Block main/controller thread until the initial IQ reserve is ready.
  // Never called from the USB callback.
  bool wait_for_bytes(std::size_t minimum, std::chrono::milliseconds timeout);
  std::uint64_t underrun_bytes() const {return underrun_bytes_.load();}
  std::uint64_t sent_bytes() const {return sent_bytes_.load();}
  std::uint64_t overflow_count() const {return overflow_count_.load();}
  struct UnderrunEvent {
    double from_first_callback_ms=0;
    std::uint64_t callback_number=0;
    std::size_t missing_bytes=0;
    std::size_t queued_before=0;
    bool closing_padding=false;
  };
  struct Snapshot {
    std::size_t queued_bytes;
    std::size_t peak_bytes;
    std::uint64_t callbacks;
    std::uint64_t underrun_bytes;
    std::uint64_t sent_bytes;
    std::uint64_t overflow_count;
    std::uint64_t underrun_events;
    std::uint64_t active_underrun_bytes;
    std::uint64_t active_underrun_events;
    std::uint64_t closing_padding_bytes;
    std::uint64_t closing_padding_events;
    bool producer_finished;
    double last_underrun_ms;
    double first_push_relative_to_callback_ms;
    std::uint64_t push_count;
  };
  std::vector<UnderrunEvent> underrun_events();
  Snapshot snapshot();
  static int callback(hackrf_transfer* t);
private:
  std::mutex mutex_;
  std::condition_variable ready_cv_;
  std::deque<std::vector<std::int8_t>> blocks_;
  std::size_t offset_=0,queued_=0,peak_=0;
  std::uint64_t callbacks_=0;
  std::uint64_t push_count_=0;
  bool first_push_seen_=false;
  std::chrono::steady_clock::time_point first_push_{};
  bool callback_clock_started_=false;
  std::chrono::steady_clock::time_point first_callback_{};
  bool producer_finished_=false;
  std::uint64_t underrun_events_total_=0;
  std::uint64_t active_underrun_events_=0,closing_padding_events_=0;
  double last_underrun_ms_=-1;
  std::vector<UnderrunEvent> underrun_event_log_;
  static const std::size_t kMaxLoggedEvents=64;
  static const std::size_t kMaxBytes=8000000; // 2 seconds at 2 MS/s IQ8
  std::atomic<std::uint64_t> underrun_bytes_{0},sent_bytes_{0},overflow_count_{0};
  std::atomic<std::uint64_t> active_underrun_bytes_{0},closing_padding_bytes_{0};
};
