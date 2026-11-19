#include "rigctl_server_cycle.hpp"
#include <chrono>
#include <iostream>

void cycle_log(const std::string& line) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  std::cerr<<line<<"\n";
}

bool PttCycle::request(bool on) {
  // Serialize even when two CAT clients submit commands concurrently.
  std::lock_guard<std::mutex> call_lock(call_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if(closing_) return false;
  if(issued_==completed_ && on==actual_) return true;
  desired_=on;
  requested_at_=std::chrono::steady_clock::now();
  const std::uint64_t serial=++issued_;
  const bool done=cv_.wait_for(lock,std::chrono::seconds(4),[&] {
    return closing_ || completed_>=serial;
  });
  if(!done && !closing_) {
    // Cancel any delayed ON request: never leave a pending PTT after CAT timeout.
    desired_=false;
    requested_at_=std::chrono::steady_clock::now();
    ++issued_;
    cv_.notify_all();
  }
  return done && !closing_ && completed_>=serial && last_ok_ && actual_==on;
}
bool PttCycle::take_pending(bool& on,std::uint64_t& serial,
                             std::chrono::steady_clock::time_point& requested_at) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(closing_ || issued_==completed_) return false;
  on=desired_;serial=issued_;requested_at=requested_at_;
  return true;
}
bool PttCycle::complete(std::uint64_t serial,bool success,bool active) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(serial!=issued_ || closing_) return false;
  actual_=active;
  last_ok_=success;
  if(!success) desired_=false;
  completed_=serial;
  cv_.notify_all();
  return true;
}
bool PttCycle::active() {
  std::lock_guard<std::mutex> lock(mutex_);return actual_;
}
void PttCycle::force_off() {
  std::lock_guard<std::mutex> lock(mutex_);
  if(closing_ || (issued_==completed_ && !actual_)) return;
  desired_=false;
  requested_at_=std::chrono::steady_clock::now();
  ++issued_;
  cv_.notify_all();
}
std::string PttCycle::state() {
  std::lock_guard<std::mutex> lock(mutex_);return state_;
}
void PttCycle::set_state(const std::string& state) {
  std::lock_guard<std::mutex> lock(mutex_);state_=state;
}
void PttCycle::close() {
  std::lock_guard<std::mutex> lock(mutex_);
  closing_=true;desired_=false;cv_.notify_all();
}
