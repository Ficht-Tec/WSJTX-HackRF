#pragma once
#include <atomic>
#include <string>
#include <thread>
class TxIqQueue;
// Reads PCM48k mono s16 from a local FIFO, writes modulated IQ only to files
// during simulated PTT. Does NOT link to HackRF hardware code.
class TxSimCycleWorker {
 public:
  TxSimCycleWorker(std::atomic<bool>& ptt,const std::string& fifo,const std::string& directory,unsigned max_ptt_seconds, TxIqQueue* usb_queue=nullptr);
  ~TxSimCycleWorker();
  bool start();
  void stop();
  bool active() const { return active_.load(); }
  bool failed() const { return failed_.load(); }
 private:
  void run();
  std::atomic<bool>& ptt_;
  std::string fifo_,directory_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> active_{false};
  std::atomic<bool> failed_{false};
  unsigned max_ptt_seconds_;
  TxIqQueue* usb_queue_=nullptr;
  std::thread thread_;
  bool own_fifo_=false;
};
