#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct RadioSettings {
  std::uint64_t dial_hz=14074000;
  int lna=16;
  int vga=16;
  int amp=0; // RF amplifier RX, boolean; default OFF
};

// The RX/DSP main thread is the sole owner of the HackRF device.
// The CAT thread only requests mutations and waits for their acknowledgement.
class RadioControl {
 public:
  explicit RadioControl(const RadioSettings& initial);
  RadioSettings current();
  bool request_frequency(std::uint64_t hz);
  bool request_lna(int gain);
  bool request_vga(int gain);
  // Queue RX gain changes while transmitting, without touching HackRF.
  bool stage_rx_lna(int gain);
  bool stage_rx_vga(int gain);
  RadioSettings staged_rx();
  bool request_amp(int enable);
  // Staged configuration only. No call to the HackRF TX hardware in phase 2.
  bool stage_tx_vga(int gain);
  int staged_tx_vga();
  int applied_tx_vga();
  bool take_tx_vga_request(int& gain);
  void confirm_tx_vga(int gain, bool success);
  void clear_tx_vga_applied();
  bool take_pending(RadioSettings& target, std::uint64_t& serial);
  void complete(std::uint64_t serial, bool success, const RadioSettings& current);
  void close();
 private:
  enum Field { FREQ, LNA, VGA, AMP };
  bool request(Field what, std::uint64_t val);
  bool stage_rx(Field what,int gain);
  std::mutex mutex_;
  std::condition_variable cv_;
  RadioSettings desired_, actual_;
  int staged_tx_vga_=0;
  int applied_tx_vga_=-1;  // -1: no TX hardware configured/active
  bool tx_vga_update_pending_=false;
  std::uint64_t issued_=0, completed_=0;
  bool result_=true, closing_=false;
};

// One-line synchronized logger for main/CAT/worker transition events.
void cycle_log(const std::string& line);

// Synchronizes rigctl PTT commands with the real RX stop/start on the main thread.
// Never controls an RF TX path; this is a strictly off-air cycle tester.
class PttCycle {
 public:
  bool request(bool on);
  bool take_pending(bool& on,std::uint64_t& serial,std::chrono::steady_clock::time_point& requested_at);
  bool complete(std::uint64_t serial,bool success,bool active);
  bool active();
  void force_off();
  std::string state();
  void set_state(const std::string& state);
  void close();
 private:
  std::mutex call_mutex_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool desired_=false,actual_=false,closing_=false,last_ok_=true;
  std::uint64_t issued_=0,completed_=0;
  std::chrono::steady_clock::time_point requested_at_;
  std::string state_="RX_STARTING";
};

class RigctlCycleServer {
 public:
  RigctlCycleServer(RadioControl& control, PttCycle& cycle, bool rf_enabled=false);
  ~RigctlCycleServer();
  bool start(unsigned port);
  void stop();
 private:
  void loop();
  std::string respond(const std::string& line, bool& terminate_client);
  bool rf_enabled_=false;
  RadioControl& control_;
  PttCycle& cycle_;
  std::atomic<bool> quit_{false};
  std::thread thread_;
  int listen_fd_=-1;
};
