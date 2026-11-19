// WSJTX-HackRF bridge: RX state diagnostics + configurable OFF-AIR PTT duration.
// No hackrf_start_tx, no RF transmission; QRadioLink independent.
#include "rigctl_server_cycle.hpp"
#include "txsim_worker_cycle.hpp"
#include "tx_iq_queue.hpp"
#include "usb_demod.hpp"
#include <libhackrf/hackrf.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <string>
#include <sstream>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
const std::size_t kMaxBlock=262144, kQueueSlots=12;
const std::uint64_t kLoOffset=10000;
volatile std::sig_atomic_t stop_requested=0;
void on_signal(int) {stop_requested=1;}
void put_u16(std::ostream& o,std::uint16_t n) {
  const char s[2]={static_cast<char>(n),static_cast<char>(n>>8)};o.write(s,2);
}
void put_u32(std::ostream& o,std::uint32_t n) {
  const char s[4]={static_cast<char>(n),static_cast<char>(n>>8),
                   static_cast<char>(n>>16),static_cast<char>(n>>24)};o.write(s,4);
}
class PcmWriter {
 public:
  explicit PcmWriter(const std::string& output):raw_(output=="-") {
    if(!raw_) {
      file_.open(output.c_str(),std::ios::binary|std::ios::trunc);
      if(file_) {file_.write("RIFF",4);put_u32(file_,0);file_.write("WAVEfmt ",8);
        put_u32(file_,16);put_u16(file_,1);put_u16(file_,1);
        put_u32(file_,48000);put_u32(file_,96000);put_u16(file_,2);put_u16(file_,16);
        file_.write("data",4);put_u32(file_,0);}
    }
  }
  bool good() const {return raw_ || static_cast<bool>(file_);}
  bool write(const std::vector<std::int16_t>& samples) {
    if(samples.empty()) return true;
    const std::size_t n=samples.size()*2;
    if(raw_) return std::fwrite(samples.data(),1,n,stdout)==n;
    file_.write(reinterpret_cast<const char*>(samples.data()),static_cast<std::streamsize>(n));
    bytes_+=n;return static_cast<bool>(file_);
  }
  bool close() {
    if(raw_) {std::fflush(stdout);return std::ferror(stdout)==0;}
    if(!file_ || bytes_>0xffffff00u) return false;
    file_.seekp(4);put_u32(file_,static_cast<std::uint32_t>(36+bytes_));
    file_.seekp(40);put_u32(file_,static_cast<std::uint32_t>(bytes_));
    file_.close();return !file_.fail();
  }
 private:
  bool raw_;std::ofstream file_;std::uint64_t bytes_=0;
};
struct Block {std::array<std::int8_t,kMaxBlock> data;std::size_t size=0;};
struct RxQueue {
  std::array<Block,kQueueSlots> blocks;
  std::mutex mutex;
  std::condition_variable cv;
  std::size_t head=0,tail=0,pending=0,peak=0,peak_interval=0;
  bool accepting=true;
  bool switching=false;
  std::uint64_t switch_dropped=0;
  std::int64_t cycle_first_us=-1;
  unsigned skip=0;
  std::uint64_t callback_bytes=0,callbacks=0,lost=0,invalid=0,tune_dropped=0;
  Clock::time_point started;
  std::int64_t first_us=-1;
};
int rx_callback(hackrf_transfer* t) {
  auto* q=static_cast<RxQueue*>(t->rx_ctx);
  if(!q || !t->buffer || t->valid_length<=0 || t->valid_length>t->buffer_length ||
     (t->valid_length%2) || static_cast<std::size_t>(t->valid_length)>kMaxBlock) {
    if(q){std::lock_guard<std::mutex> lock(q->mutex);++q->invalid;}return 1;
  }
  std::lock_guard<std::mutex> lock(q->mutex);
  ++q->callbacks;q->callback_bytes+=static_cast<std::uint64_t>(t->valid_length);
  if(q->first_us<0) q->first_us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-q->started).count();
  if(q->cycle_first_us<0) q->cycle_first_us=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-q->started).count();
  if(!q->accepting) { if(q->switching) ++q->switch_dropped; else ++q->tune_dropped; return 0; }
  if(q->skip) {--q->skip;++q->tune_dropped;return 0;}
  if(q->pending==kQueueSlots) {++q->lost;return 0;}
  Block& b=q->blocks[q->head];b.size=static_cast<std::size_t>(t->valid_length);
  std::memcpy(b.data.data(),t->buffer,b.size);
  q->head=(q->head+1)%kQueueSlots;++q->pending;
  q->peak=std::max(q->peak,q->pending);
  q->peak_interval=std::max(q->peak_interval,q->pending);
  q->cv.notify_one();return 0;
}
bool pop_block(RxQueue& q,Block& scratch,unsigned wait_ms) {
  std::unique_lock<std::mutex> lock(q.mutex);
  if(!q.pending && wait_ms) q.cv.wait_for(lock,std::chrono::milliseconds(wait_ms));
  if(!q.pending) return false;
  const Block& b=q.blocks[q.tail];scratch.size=b.size;
  std::memcpy(scratch.data.data(),b.data.data(),b.size);
  q.tail=(q.tail+1)%kQueueSlots;--q.pending;return true;
}
bool check(const char* label,int code) {
  if(code==HACKRF_SUCCESS) return true;
  std::cerr<<label<<" => "<<hackrf_error_name(static_cast<hackrf_error>(code))<<" ("<<code<<")\n";
  return false;
}
bool apply(hackrf_device* dev,RxQueue& queue,UsbDemod& dsp,
           const RadioSettings& requested,RadioSettings& actual) {
  if(requested.dial_hz!=actual.dial_hz) {
    {std::lock_guard<std::mutex> lock(queue.mutex);queue.accepting=false;}
    const int rc=hackrf_set_freq(dev,requested.dial_hz-kLoOffset);
    {std::lock_guard<std::mutex> lock(queue.mutex);
      queue.tune_dropped+=queue.pending;
      queue.tail=queue.head;queue.pending=0;
      queue.skip=3; // ~0.197 s settle at 2 MS/s, 262144 bytes/callback
      queue.accepting=true;
    }
    if(!check("CAT retune",rc)) return false;
    dsp=UsbDemod(); // oscillator and filters must not carry state across bands
    actual.dial_hz=requested.dial_hz;
    std::cerr<<"CAT: dial="<<actual.dial_hz<<" Hz, LO="<<actual.dial_hz-kLoOffset<<" Hz\n";
  }
  if(requested.lna!=actual.lna) {
    if(!check("set LNA",hackrf_set_lna_gain(dev,requested.lna))) return false;
    actual.lna=requested.lna;
    std::cerr<<"CAT: LNA="<<actual.lna<<" dB\n";
  }
  if(requested.amp!=actual.amp) {
    if(!check("set RX AMP",hackrf_set_amp_enable(dev,static_cast<uint8_t>(requested.amp)))) return false;
    actual.amp=requested.amp;
    std::cerr<<"CAT: RX AMP="<<actual.amp<<"\n";
  }
  if(requested.vga!=actual.vga) {
    if(!check("set VGA",hackrf_set_vga_gain(dev,requested.vga))) return false;
    actual.vga=requested.vga;
    std::cerr<<"CAT: VGA="<<actual.vga<<" dB\n";
  }
  return true;
}
bool number(const std::string& str,std::uint64_t& value) {
  if(str.empty()) return false;
  for(char c:str) if(c<'0'||c>'9') return false;
  try { const unsigned long long v=std::stoull(str);
    value=static_cast<std::uint64_t>(v);return true; }
  catch(...) {return false;}
}
void help() {
  std::cerr<<"Uso: bridge-hackrf-cat --live SEGUNDOS SALIDA [--freq-hz HZ] [--lna DB] [--vga DB] [--amp 0|1] [--port TCP] [--tx-max-sec N]\n"
           <<"  SEGUNDOS=0: recibir hasta Ctrl+C. SALIDA=-: PCM s16le 48kHz por stdout.\n"
           <<"  --tx-fifo ruta_fifo --tx-dir directorio_de_IQ. TX SIM por defecto.\n"
           <<"  LNA: 0..40 paso 8 dB; VGA: 0..62 paso 2 dB; AMP RX 0|1.\n"
           "  CAT: \\set_tx_vga 0..47 prepara la ganancia para el siguiente PTT RF.\n"
           <<"  --tx-max-sec: 0 desactiva el limite temporal; 5..2400 s lo habilita.\n"
           <<"  --rf-enable --load-50ohm-confirmed: ambos requeridos para RF; sin ellos TX_SIM.\n";
}
}
int main(int argc,char** argv) {
  if(argc<4 || std::strcmp(argv[1],"--live")!=0) {help();return 2;}
  std::uint64_t seconds=0,port=4532,tx_max_sec=29;
  bool rf_enabled=false,load_confirmed=false;
  std::string fifo_path="config/tx-audio.pcm";
  std::string tx_dir="logs";
  if(!number(argv[2],seconds) || seconds>86400) {help();return 2;}
  const std::string outfile=argv[3];
  RadioSettings setting;
  for(int i=4;i<argc;) {
    if(std::strcmp(argv[i],"--rf-enable")==0){rf_enabled=true;++i;continue;}
    if(std::strcmp(argv[i],"--load-50ohm-confirmed")==0){load_confirmed=true;++i;continue;}
    if(i+1>=argc) {help();return 2;}
    i+=2;
    if(std::strcmp(argv[i-2],"--tx-fifo")==0) {fifo_path=argv[i-1];continue;}
    if(std::strcmp(argv[i-2],"--tx-dir")==0) {tx_dir=argv[i-1];continue;}
    std::uint64_t n=0;
    if(!number(argv[i-1],n)) {help();return 2;}
    if(std::strcmp(argv[i-2],"--freq-hz")==0) setting.dial_hz=n;
    else if(std::strcmp(argv[i-2],"--lna")==0) setting.lna=static_cast<int>(n);
    else if(std::strcmp(argv[i-2],"--vga")==0) setting.vga=static_cast<int>(n);
    else if(std::strcmp(argv[i-2],"--amp")==0) setting.amp=static_cast<int>(n);
    else if(std::strcmp(argv[i-2],"--port")==0) port=n;
    else if(std::strcmp(argv[i-2],"--tx-max-sec")==0) tx_max_sec=n;
    else {help();return 2;}
  }
  if(setting.dial_hz<1010000 || setting.dial_hz>6000000000ULL ||
     setting.lna<0 || setting.lna>40 || setting.lna%8 ||
     setting.vga<0 || setting.vga>62 || setting.vga%2 ||
     (setting.amp!=0 && setting.amp!=1) || port==0 || port>65535 ||
     (tx_max_sec!=0 && (tx_max_sec<5 || tx_max_sec>2400))) {
    help();return 2;
  }
  if(rf_enabled!=load_confirmed) {std::cerr<<"RF: both flags required\n";return 2;}
  std::signal(SIGINT,on_signal);
  std::signal(SIGTERM,on_signal);
  std::signal(SIGPIPE,SIG_IGN);
  PcmWriter writer(outfile);
  if(!writer.good()) {std::cerr<<"No puedo abrir "<<outfile<<"\n";return 1;}
  if(!check("hackrf_init",hackrf_init())) return 1;
  hackrf_device* dev=nullptr;
  if(!check("hackrf_open",hackrf_open(&dev))) {hackrf_exit();return 1;}
  const bool configured=check("sample rate",hackrf_set_sample_rate(dev,2000000.0)) &&
    check("initial frequency",hackrf_set_freq(dev,setting.dial_hz-kLoOffset)) &&
    check("RF amplifier initial",hackrf_set_amp_enable(dev,static_cast<uint8_t>(setting.amp))) &&
    check("antenna bias OFF",hackrf_set_antenna_enable(dev,0)) &&
    check("initial LNA",hackrf_set_lna_gain(dev,setting.lna)) &&
    check("initial VGA",hackrf_set_vga_gain(dev,setting.vga));
  std::unique_ptr<RxQueue> queue(new RxQueue());
  std::unique_ptr<Block> scratch(new Block());
  UsbDemod dsp;
  std::vector<std::int16_t> pcm;
  RadioControl control(setting);
  std::atomic<bool> simulated_ptt{false};
  PttCycle cycle;
  RigctlCycleServer cat(control,cycle,rf_enabled);
  TxIqQueue tx_queue;
  std::uint64_t last_tx_cb=0,last_tx_sent=0,last_tx_underrun=0,last_tx_overflows=0;
  bool previous_tx_running=false;
  TxSimCycleWorker txsim(simulated_ptt,fifo_path,tx_dir,static_cast<unsigned>(tx_max_sec),rf_enabled?&tx_queue:nullptr);
  bool tx_running=false;
  bool started=false,good=configured;
  bool rx_running=false,awaiting_first_callback=false;
  std::uint64_t cycle_count=0;
  Clock::time_point tx_started=Clock::now();
  if(good) {
    cycle.set_state("RX_STARTING");
    queue->started=Clock::now();
    started=check("hackrf_start_rx",hackrf_start_rx(dev,rx_callback,queue.get()));
    good=started;
    rx_running=started;
    awaiting_first_callback=started;
    if(!started) cycle.set_state("FAULT");
  }
  if(good && !txsim.start()) good=false;
  if(good && !cat.start(static_cast<unsigned>(port))) good=false;
  std::uint64_t processed=0;
  std::uint64_t last_report_lost=0, last_report_tune=0, last_report_callbacks=0;
  if(good) {
    std::cerr<<"WSJTX-HackRF bridge | RX-CYCLE + PTT "<<(rf_enabled?"RF":"SIM")<<" | dial="<<setting.dial_hz
             <<"Hz | rate=2 MS/s | audio 48kHz | tx_max_sec="<<tx_max_sec<<" | "
             <<(seconds ? std::to_string(seconds)+"s" : "continuo")<<"\n";
    const Clock::time_point deadline=queue->started+std::chrono::seconds(seconds);
    Clock::time_point report=queue->started+std::chrono::seconds(1);
    while(!stop_requested && (!seconds || Clock::now()<deadline)) {
      // Two independent reasons to leave TX_SIM: time budget or worker failure.
      // The main RX/CAT thread alone owns HackRF RX stop/restart.
      if(!rx_running && cycle.active()) {
        if(txsim.failed() || !simulated_ptt.load()) {
          cycle_log("RX_CYCLE RECORDER_FAULT: forcing PTT OFF and restoring RX; RF_TX=DISABLED");
          cycle.force_off();
        } else if(tx_max_sec!=0 && Clock::now()-tx_started>=std::chrono::seconds(tx_max_sec)) {
          std::ostringstream msg;
          msg<<"RX_CYCLE TX_LIMIT: PTT exceeded "<<tx_max_sec
             <<" s, forcing OFF, RF_TX=DISABLED";
          cycle_log(msg.str());
          cycle.force_off();
        }
      }
      // The main thread is the sole owner of the HackRF device and its mode.
      // The CAT thread waits until the operation has finished before ACKing T n.
      bool want_tx=false;
      std::uint64_t cycle_rev=0;
      Clock::time_point request_at;
      if(cycle.take_pending(want_tx,cycle_rev,request_at)) {
        const auto change_started=Clock::now();
        if(want_tx && rx_running) {
          // A failed recorder must not cause RX to stop for another TX request.
          if(txsim.failed()) {
            cycle_log("RX_CYCLE REJECT TX: recorder failed; RX remains active, RF_TX=DISABLED");
            cycle.complete(cycle_rev,false,false);
            continue;
          }
          cycle.set_state("RX_STOPPING");
          {
            std::lock_guard<std::mutex> lk(queue->mutex);
            queue->accepting=false;
            queue->switching=true;
          }
          // Physical RX stops; RF transmitter is NEVER started.
          const int rc=hackrf_stop_rx(dev);
          const auto stopped_at=Clock::now();
          if(rc!=HACKRF_SUCCESS || hackrf_is_streaming(dev)==HACKRF_TRUE) {
            check("RX_CYCLE stop_rx",rc);
            std::cerr<<"RX_CYCLE FATAL: stop_rx failed or device still streaming; NO TX.\n";
            simulated_ptt=false;
            cycle.set_state("FAULT");
            cycle.complete(cycle_rev,false,false);
            good=false;break;
          }
          rx_running=false;
          awaiting_first_callback=false; // an interrupted RX start is not RX_READY
          {
            std::lock_guard<std::mutex> lk(queue->mutex);
            queue->switch_dropped+=queue->pending;
            queue->pending=0;queue->tail=queue->head;
            queue->skip=0;
          }
          if(rf_enabled) tx_queue.clear();
          last_tx_cb=last_tx_sent=last_tx_underrun=last_tx_overflows=0;
          previous_tx_running=false;
          simulated_ptt=true; // Allow the existing PCM -> IQ pipeline after RX stops.
          // Wait until file recorder is armed; do not ACK CAT before it is ready.
          const auto arm_at=Clock::now();
          while(!txsim.active() && Clock::now()-arm_at<std::chrono::milliseconds(500))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          if(!txsim.active()) {
            simulated_ptt=false;
            cycle.set_state("FAULT");
            cycle.complete(cycle_rev,false,false);
            cycle_log("RX_CYCLE FATAL: TX SIM recorder did not arm; RX paused, RF_TX=DISABLED");
            good=false;break;
          }
          if(rf_enabled) {
            // F4g: prime the real IQ FIFO BEFORE starting the HackRF USB TX callback.
            // 327680 bytes is ~81.9 ms at 2 MS/s IQ8: one 262144-byte
            // USB transfer plus 16.4 ms of margin. No samples are fabricated,
            // discarded or reordered, and no sleeps occur in the USB callback.
            // Fail closed if input stalls: do not start RF with an empty FIFO.
            constexpr std::size_t kTxInitialBytes=327680;
            const auto prime_begin=Clock::now();
            const bool primed=tx_queue.wait_for_bytes(kTxInitialBytes,
                                                      std::chrono::milliseconds(180));
            const auto prime_end=Clock::now();
            const auto prime_snap=tx_queue.snapshot();
            {
              std::ostringstream msg;
              msg<<"TX_PRIME target_bytes="<<kTxInitialBytes
                 <<" queued_bytes="<<prime_snap.queued_bytes
                 <<" wait_ms="<<std::chrono::duration<double,std::milli>(prime_end-prime_begin).count()
                 <<" result="<<(primed?"READY":"TIMEOUT");
              cycle_log(msg.str());
            }
            if(!primed) {
              simulated_ptt=false;
              cycle.set_state("FAULT");cycle.complete(cycle_rev,false,false);
              cycle_log("TX_PRIME FAILED: RF not started; process exiting, restart RX required");
              good=false;break;
            }
            // Start USB TX only after HackRF RX is confirmed stopped.
            // Initial gain comes from CAT; subsequent changes are applied in the main loop.
            const int txvga=control.staged_tx_vga();
            const bool tx_configured=check("TX bias OFF",hackrf_set_antenna_enable(dev,0)) &&
              check("TX amplifier OFF",hackrf_set_amp_enable(dev,0)) &&
              check("TX frequency",hackrf_set_freq(dev,setting.dial_hz-kLoOffset)) &&
              check("TX gain",hackrf_set_txvga_gain(dev,static_cast<uint32_t>(txvga)));
            if(!tx_configured || !check("hackrf_start_tx",tx_configured?
                    hackrf_start_tx(dev,TxIqQueue::callback,&tx_queue):-1)) {
              simulated_ptt=false;
              cycle.set_state("FAULT");cycle.complete(cycle_rev,false,false);
              cycle_log("RF TX START FAILED: RF OFF; process exiting (relaunch RX required)");
              good=false;break;
            }
            tx_running=true;
            control.confirm_tx_vga(txvga,true);
            cycle.set_state("TX_RF_ACTIVE");
            cycle_log("RF_TX=ACTIVE TX_VGA="+std::to_string(txvga)+" (load confirmed)");
          } else cycle.set_state("TX_SIM_OFF_AIR");
          tx_started=Clock::now();
          const bool acked=cycle.complete(cycle_rev,true,true);
          if(!acked) simulated_ptt=false; // timed-out CAT request was cancelled
          ++cycle_count;
          {
            std::ostringstream msg;
            msg<<"RX_CYCLE ON #"<<cycle_count
               <<" stop_rx_ms="<<std::chrono::duration<double,std::milli>(stopped_at-change_started).count()
               <<" recorder_arm_ms="<<std::chrono::duration<double,std::milli>(Clock::now()-arm_at).count()
               <<" command_to_gate_ms="<<std::chrono::duration<double,std::milli>(Clock::now()-request_at).count()
               <<" gate="<<(acked?(rf_enabled?"TX_RF":"TX_SIM"):"OFF")
               <<" RF_TX="<<(rf_enabled?"LAB":"DISABLED");
            cycle_log(msg.str());
          }
        } else if(!want_tx && !rx_running) {
          cycle.set_state("RX_STARTING");
          // F4h: close the producer BEFORE stopping USB TX. The worker's
          // active_ flag drops only after mod->flush() has been pushed to IQ USB.
          // During this bounded finalization RF remains enabled intentionally.
          simulated_ptt=false;
          const auto flush_begin=Clock::now();
          while(txsim.active() && !txsim.failed() &&
                Clock::now()-flush_begin<std::chrono::milliseconds(1500))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          const bool flushed=!txsim.active() && !txsim.failed();
          const auto producer_done=Clock::now();
          bool drained=false;
          std::size_t queued_on_stop=0;
          double drain_ms=0.0;
          if(tx_running && flushed) {
            // No more producer pushes are possible after active_=false.
            // Stop only after all queued samples have entered USB callbacks.
            while(Clock::now()-producer_done<std::chrono::milliseconds(1000)) {
              const auto snap=tx_queue.snapshot();
              if(snap.queued_bytes==0) {drained=true;break;}
              if(hackrf_is_streaming(dev)!=HACKRF_TRUE) break;
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            drain_ms=std::chrono::duration<double,std::milli>(Clock::now()-producer_done).count();
            // One USB callback period (~65.5 ms) as a bounded transport margin:
            // the last callback may have been filled but is not yet on air.
            if(drained && hackrf_is_streaming(dev)==HACKRF_TRUE)
              std::this_thread::sleep_for(std::chrono::milliseconds(70));
          }
          if(tx_running) {
            queued_on_stop=tx_queue.snapshot().queued_bytes;
            // Even on timeout/fault stop RF, never leave PTT active indefinitely.
            const int stop_rc=hackrf_stop_tx(dev);
            if(stop_rc!=HACKRF_SUCCESS || hackrf_is_streaming(dev)==HACKRF_TRUE) {
              check("hackrf_stop_tx",stop_rc);
              cycle.set_state("FAULT");cycle.complete(cycle_rev,false,false);
              cycle_log("RF_TX STOP FAILED: refusing RX restart");good=false;break;
            }
            tx_running=false;
            control.clear_tx_vga_applied();
            cycle_log("TX_FINAL_DRAIN producer_finished="+std::to_string(flushed?1:0)+
              " drained="+std::to_string(drained?1:0)+
              " queued_on_stop="+std::to_string(queued_on_stop)+
              " producer_wait_ms="+std::to_string(std::chrono::duration<double,std::milli>(producer_done-flush_begin).count())+
              " drain_wait_ms="+std::to_string(drain_ms)+
              " success="+std::to_string((flushed && drained && queued_on_stop==0)?1:0));
            // Printed only AFTER hackrf_stop_tx: no I/O from the USB callback.
            const auto tx_final=tx_queue.snapshot();
            cycle_log("TX_UNDERRUN_SUMMARY events="+std::to_string(tx_final.underrun_events)+
              " first_64="+std::to_string(tx_queue.underrun_events().size())+
              " last_ms_from_first_callback="+std::to_string(tx_final.last_underrun_ms));
            cycle_log("TX_QUALITY active_underrun_events="+std::to_string(tx_final.active_underrun_events)+
              " active_underrun_bytes="+std::to_string(tx_final.active_underrun_bytes)+
              " closing_padding_events="+std::to_string(tx_final.closing_padding_events)+
              " closing_padding_bytes="+std::to_string(tx_final.closing_padding_bytes)+
              " producer_finished="+std::to_string(tx_final.producer_finished?1:0)+
              " total_usb_zero_bytes="+std::to_string(tx_final.underrun_bytes));
            for(const auto& e:tx_queue.underrun_events()) {
              std::ostringstream line;
              line<<"TX_UNDERRUN_EVENT ms_from_first_callback="<<e.from_first_callback_ms
                  <<" callback="<<e.callback_number
                  <<" missing="<<e.missing_bytes
                  <<" queued_after_consume="<<e.queued_before
                  <<" kind="<<(e.closing_padding?"CLOSING_PADDING":"ACTIVE_UNDERRUN");
              cycle_log(line.str());
            }
            cycle_log("TX_START_CORRELATION first_push_minus_callback_ms="+
              std::to_string(tx_final.first_push_relative_to_callback_ms)+
              " push_count="+std::to_string(tx_final.push_count));
            cycle_log("RF_TX=OFF iq_sent="+std::to_string(tx_queue.sent_bytes())+
              " underrun_bytes="+std::to_string(tx_queue.underrun_bytes())+
              " overflows="+std::to_string(tx_queue.overflow_count()));
          }
          if(!flushed) cycle_log("TX_FINAL_DRAIN WARNING: producer flush incomplete; RF stopped");
          if(tx_running==false && !drained && rf_enabled)
            cycle_log("TX_FINAL_DRAIN WARNING: USB queue not drained; RF stopped");
          dsp=UsbDemod(); // never carry filters/phase across stop/start RX
          {
            std::lock_guard<std::mutex> lk(queue->mutex);
            queue->pending=0;queue->tail=queue->head;
            queue->skip=3;
            queue->started=Clock::now();
            queue->cycle_first_us=-1;
            queue->accepting=true;
            queue->switching=false;
          }
          // Re-establish RX configuration with RX amp and bias disabled.
          const bool configured_rx=check("RX_CYCLE sample rate",hackrf_set_sample_rate(dev,2000000.0)) &&
            check("RX_CYCLE RX AMP",hackrf_set_amp_enable(dev,static_cast<uint8_t>(setting.amp))) &&
            check("RX_CYCLE safe bias OFF",hackrf_set_antenna_enable(dev,0)) &&
            check("RX_CYCLE RX frequency",hackrf_set_freq(dev,setting.dial_hz-kLoOffset)) &&
            check("RX_CYCLE RX LNA",hackrf_set_lna_gain(dev,setting.lna)) &&
            check("RX_CYCLE RX VGA",hackrf_set_vga_gain(dev,setting.vga));
          const auto restart_begin=Clock::now();
          const int rc=configured_rx?hackrf_start_rx(dev,rx_callback,queue.get()):-9999;
          const auto restart_end=Clock::now();
          if(!configured_rx || rc!=HACKRF_SUCCESS) {
            if(configured_rx) check("RX_CYCLE restart RX",rc);
            std::cerr<<"RX_CYCLE FATAL: RX restart failed; NO TX, exiting safely.\n";
            cycle.set_state("FAULT");
            cycle.complete(cycle_rev,false,false);
            good=false;break;
          }
          rx_running=true;
          awaiting_first_callback=true;
          // CAT T 0 acknowledges that the RX stream was re-started.
          // RX_READY is deliberately deferred until the first USB callback.
          cycle.complete(cycle_rev,true,false);
          {
            std::ostringstream msg;
            msg<<"RX_CYCLE OFF #"<<cycle_count
               <<" recorder_flush_ms="<<std::chrono::duration<double,std::milli>(restart_begin-flush_begin).count()
               <<" start_rx_ms="<<std::chrono::duration<double,std::milli>(restart_end-restart_begin).count()
               <<" command_to_rx_started_ms="<<std::chrono::duration<double,std::milli>(restart_end-request_at).count()
               <<" wait_first_callback=1 RF_TX=DISABLED";
            cycle_log(msg.str());
          }
        } else {
          // Idempotent request (repeated T 1/T 0), no stream transition.
          cycle.complete(cycle_rev,true,want_tx);
        }
      }
      // Live TX VGA: the main thread owns all HackRF control calls.
      // During RX the latest CAT value stays staged for the next PTT.
      if(tx_running) {
        int live_gain=0;
        if(control.take_tx_vga_request(live_gain)) {
          const int rc=hackrf_set_txvga_gain(dev,static_cast<uint32_t>(live_gain));
          control.confirm_tx_vga(live_gain,rc==HACKRF_SUCCESS);
          if(rc==HACKRF_SUCCESS) {
            cycle_log("RF_TX VGA LIVE applied="+std::to_string(live_gain));
          } else {
            cycle_log("RF_TX VGA LIVE FAILED: rc="+std::to_string(rc)+" forcing PTT OFF");
            cycle.force_off();
          }
        }
      }
      RadioSettings want;
      std::uint64_t rev=0;
      // Keep RX gains staged throughout TX, including the transition back to RX.
      // Apply them only when the receiver is actually running.
      if(rx_running && !cycle.active() && control.take_pending(want,rev)) {
        const bool applied=apply(dev,*queue,dsp,want,setting);
        control.complete(rev,applied,setting);
      }
      if(awaiting_first_callback && rx_running) {
        std::int64_t first=-1;
        {
          std::lock_guard<std::mutex> lk(queue->mutex);
          first=queue->cycle_first_us;
        }
        if(first>=0) {
          // USB callback has actually arrived; RX_READY never means FT8 decoded.
          cycle.set_state("RX_READY");
          {
            std::ostringstream msg;
            msg<<"RX_CYCLE RX_READY first_rx_callback_ms="<<double(first)/1000.0
               <<" RF_TX=DISABLED";
            cycle_log(msg.str());
          }
          awaiting_first_callback=false;
        }
      }
      if(tx_running && hackrf_is_streaming(dev)!=HACKRF_TRUE) {
        cycle_log("RF_TX UNEXPECTED STOP: requesting OFF");
        cycle.force_off();
      }
      if(rx_running && pop_block(*queue,*scratch,20)) {
        dsp.process(scratch->data.data(),scratch->size,pcm);
        processed+=scratch->size;
        if(!writer.write(pcm)) {std::cerr<<"Error escribiendo PCM (tubería cerrada?)\n";good=false;break;}
      }
      if(rx_running && hackrf_is_streaming(dev)!=HACKRF_TRUE) {
        std::cerr<<"HackRF RX terminada inesperadamente\n";good=false;break;
      }
      if(Clock::now()>=report) {
        // Take a short snapshot without holding the USB callback mutex while logging.
        std::uint64_t callbacks,lost,tune_drop,switch_drop;
        std::size_t pending,peak,peak_interval;
        {
          std::lock_guard<std::mutex> lock(queue->mutex);
          callbacks=queue->callbacks;
          lost=queue->lost;
          tune_drop=queue->tune_dropped;
          switch_drop=queue->switch_dropped;
          pending=queue->pending;
          peak=queue->peak;
          peak_interval=queue->peak_interval;
          queue->peak_interval=pending;
        }
        std::cerr<<"RX callbacks="<<callbacks
                 <<" cb_interval="<<(callbacks-last_report_callbacks)
                 <<" lost="<<lost<<" lost_interval="<<(lost-last_report_lost)
                 <<" tune_drop="<<tune_drop<<" tune_interval="<<(tune_drop-last_report_tune)
                 <<" switch_drop="<<switch_drop<<" state="<<cycle.state()
                 <<" queue="<<pending<<"/"<<kQueueSlots
                 <<" peak="<<peak<<" peak_interval="<<peak_interval
                 <<" pcm="<<dsp.samples_out()<<"\n";
        if(tx_running) {
          const auto tx=tx_queue.snapshot();
          const double elapsed=std::chrono::duration<double>(Clock::now()-tx_started).count();
          std::cerr<<"TX_QUEUE elapsed_s="<<elapsed
                   <<" queued="<<tx.queued_bytes<<"/8000000"
                   <<" peak="<<tx.peak_bytes
                   <<" callbacks="<<tx.callbacks
                   <<" cb_interval="<<(tx.callbacks-last_tx_cb)
                   <<" iq_sent="<<tx.sent_bytes
                   <<" iq_interval="<<(tx.sent_bytes-last_tx_sent)
                   <<" underrun_bytes="<<tx.underrun_bytes
                   <<" underrun_interval="<<(tx.underrun_bytes-last_tx_underrun)
                   <<" overflows="<<tx.overflow_count
                   <<" overflow_interval="<<(tx.overflow_count-last_tx_overflows)
                   <<" underrun_events="<<tx.underrun_events
                   <<" last_underrun_ms="<<tx.last_underrun_ms<<"\n";
          last_tx_cb=tx.callbacks;
          last_tx_sent=tx.sent_bytes;
          last_tx_underrun=tx.underrun_bytes;
          last_tx_overflows=tx.overflow_count;
        }
        last_report_callbacks=callbacks;
        last_report_lost=lost;
        last_report_tune=tune_drop;
        report=Clock::now()+std::chrono::seconds(1);
      }
    }
  }
  cat.stop();
  if(tx_running) {
    if(!check("hackrf_stop_tx on exit",hackrf_stop_tx(dev))) good=false;
    tx_running=false;
  }
  simulated_ptt=false;
  txsim.stop();
  if(rx_running) {
    if(!check("hackrf_stop_rx",hackrf_stop_rx(dev))) good=false;
    // Drain only if RX was actually running.
    while(pop_block(*queue,*scratch,0)) {
      dsp.process(scratch->data.data(),scratch->size,pcm);processed+=scratch->size;
      if(!writer.write(pcm)) {good=false;break;}
    }
  }
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    std::cerr<<"Resumen: callbacks="<<queue->callbacks
             <<" bytes_usb="<<queue->callback_bytes<<" bytes_dsp="<<processed
             <<" lost_blocks="<<queue->lost<<" tune_discarded="<<queue->tune_dropped
             <<" switch_discarded="<<queue->switch_dropped<<" rx_cycles="<<cycle_count
             <<" invalid="<<queue->invalid<<" first_callback_us="<<queue->first_us
             <<" peak="<<queue->peak<<"/"<<kQueueSlots
             <<" audio_samples_since_last_retune="<<dsp.samples_out()<<"\n";
    if(queue->lost || queue->invalid) good=false;
  }
  if(!writer.close()) good=false;
  if(!check("hackrf_close",hackrf_close(dev))) good=false;
  if(!check("hackrf_exit",hackrf_exit())) good=false;
  return good?0:1;
}
