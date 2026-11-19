#include "txsim_worker_cycle.hpp"
#include "rigctl_server_cycle.hpp"
#include "usb_tx_modulator.hpp"
#include "tx_iq_queue.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

TxSimCycleWorker::TxSimCycleWorker(std::atomic<bool>& ptt,const std::string& fifo,const std::string& directory,unsigned max_ptt_seconds, TxIqQueue* usb_queue)
 :ptt_(ptt),fifo_(fifo),directory_(directory),max_ptt_seconds_(max_ptt_seconds),usb_queue_(usb_queue) {}
TxSimCycleWorker::~TxSimCycleWorker(){stop();}
bool TxSimCycleWorker::start(){
 struct stat st;
 if(lstat(directory_.c_str(),&st)!=0 || !S_ISDIR(st.st_mode)) {
   std::cerr<<"TX SIM: no existe directorio "<<directory_<<"\n";return false;
 }
 if(lstat(fifo_.c_str(),&st)!=0) {
   if(errno!=ENOENT || mkfifo(fifo_.c_str(),0600)!=0) {
     std::cerr<<"TX SIM: no puedo crear FIFO "<<fifo_<<": "<<strerror(errno)<<"\n";return false;
   }
   own_fifo_=true;
 } else if(!S_ISFIFO(st.st_mode)) {
   std::cerr<<"TX SIM: la ruta no es FIFO: "<<fifo_<<"\n";return false;
 }
 quit_=false;
 failed_=false;
 thread_=std::thread(&TxSimCycleWorker::run,this);
 return true;
}
void TxSimCycleWorker::stop(){
 quit_=true;
 ptt_=false;
 if(thread_.joinable()) thread_.join();
 // Leave FIFO for debugging; never remove a pre-existing user object.
}
void TxSimCycleWorker::run(){
 const int fd=open(fifo_.c_str(),O_RDONLY|O_NONBLOCK|O_CLOEXEC);
 if(fd<0){std::cerr<<"TX SIM: cannot open FIFO\n";failed_=true;ptt_=false;return;}
 std::array<unsigned char,8192> data;
 std::array<std::int16_t,4096> pcm;
 bool was_on=false;
 std::ofstream file;
 std::ofstream pcm_file;
 std::unique_ptr<UsbTxModulator> mod;
 std::uint64_t diag_samples=0, diag_nonzero=0;
 std::uint64_t diag_first=std::numeric_limits<std::uint64_t>::max(), diag_last=0;
 auto ptt_started=std::chrono::steady_clock::now();
 double diag_first_wall_ms=-1.0;

 unsigned ordinal=0;
 bool held_byte=false;unsigned char previous=0;
  std::vector<std::int8_t> iq;
  std::uint64_t diag_pushes=0;
  double diag_first_push_ms=-1.0,diag_max_push_gap_ms=0.0;
  double diag_max_mod_ms=0.0,diag_max_disk_ms=0.0;
  std::chrono::steady_clock::time_point diag_previous_push{};
 auto started=std::chrono::steady_clock::now();
 auto last_fifo_data=started;
 while(!quit_.load()){
   bool on=ptt_.load();
   if(on && failed_.load()) {ptt_=false;on=false;}
   if(on && !was_on){
     const std::time_t t=std::time(nullptr);
     std::tm utc={};gmtime_r(&t,&utc);
     std::ostringstream name;
     name<<directory_<<"/txsim-"<<std::put_time(&utc,"%Y%m%d-%H%M%S")
         <<"-"<<getpid()<<"-"<<std::setw(3)<<std::setfill('0')<<(++ordinal)<<".iq";
     file.open(name.str().c_str(),std::ios::binary|std::ios::trunc);
     if(!file){std::cerr<<"TX SIM: no puedo escribir IQ\n";failed_=true;ptt_=false;on=false;}
     else{
       active_=true;
       // Diagnostic only: write exactly the PCM samples passed into the modulator.
       // This is not a second PulseAudio capture and does not change PTT or DSP.
       std::string pcm_name=name.str();
       pcm_name.replace(pcm_name.size()-3,3,".pcm");
       pcm_file.open(pcm_name.c_str(),std::ios::binary|std::ios::trunc);
       if(!pcm_file) {
         std::cerr<<"TX DIAG: no puedo abrir PCM espejo "<<pcm_name<<"; abortando TX sim\n";
         failed_=true;ptt_=false;on=false;
         file.close();active_=false;
       } else {
         std::cerr<<"TX DIAG: PCM real de FIFO -> "<<pcm_name<<"\n";
         diag_samples=diag_nonzero=0;
         diag_first=std::numeric_limits<std::uint64_t>::max();diag_last=0;
          diag_first_wall_ms=-1.0;
          diag_pushes=0;diag_first_push_ms=-1.0;
          diag_max_push_gap_ms=0.0;diag_max_mod_ms=0.0;diag_max_disk_ms=0.0;
         mod.reset(new UsbTxModulator());
         std::cerr<<(usb_queue_?"PTT=1 IQ -> USB queue + diagnostic file: ":"PTT=1 SIMULADO, SIN RF. IQ -> ")<<name.str()<<"\n";
         started=std::chrono::steady_clock::now();
         ptt_started=started;
         last_fifo_data=started;
       }
     }
   }
   // Secondary time guard: main controller should force OFF 2 s earlier.
   // This is not a hardware RF watchdog; RF TX is absent in this program.
   if(on && max_ptt_seconds_!=0 && std::chrono::steady_clock::now()-started>
        std::chrono::seconds(max_ptt_seconds_+2U)){
     std::cerr<<"TX SIM: limite secundario "<<max_ptt_seconds_+2U
              <<" s superado; abortando grabacion SIN RF\n";
     failed_=true;ptt_=false;on=false;
   }
   if(!on && was_on){
     if(mod && file){
       mod->flush(iq);
       // F4h: deliver interpolation tail to the live RF queue BEFORE signaling
       // active_=false. The controller waits for this producer acknowledgement.
       const std::size_t flush_bytes=iq.size();
       bool flush_queued=true;
       if(usb_queue_) {
         flush_queued=usb_queue_->finish(iq);
         if(!flush_queued) {
           std::cerr<<"TX USB: final IQ flush queue overflow\n";
           failed_=true;
         }
       }
       file.write(reinterpret_cast<const char*>(iq.data()),static_cast<std::streamsize>(iq.size()));
       if(!file) {std::cerr<<"TX SIM: fallo de escritura IQ en flush\n";failed_=true;}
       std::cerr<<"TX_FINAL_FLUSH bytes="<<flush_bytes
                <<" queued="<<(usb_queue_ && flush_queued?flush_bytes:0)
                <<" success="<<((flush_queued && file.good())?1:0)<<"\n";
       std::cerr<<"PTT=0 SIM: audio_samples="<<mod->audio_samples()
                <<" iq_samples="<<mod->iq_samples()<<"\n";}
     if(file.is_open()) {file.close();if(file.fail()) failed_=true;}
     if(pcm_file.is_open()) {pcm_file.close();if(pcm_file.fail()) failed_=true;}
     std::cerr<<"TX DIAG: pcm_samples="<<diag_samples
              <<" nonzero="<<diag_nonzero;
     if(diag_first!=std::numeric_limits<std::uint64_t>::max())
       std::cerr<<" first_nonzero_at="<<(double(diag_first)/48000.0)
                <<"s last_nonzero_at="<<(double(diag_last)/48000.0)<<"s";
     else std::cerr<<" (todo silencio)";
     std::cerr<<" first_nonzero_wall_ms="<<diag_first_wall_ms;
     std::cerr<<" ptt_duration_wall="
              <<std::chrono::duration<double>(std::chrono::steady_clock::now()-ptt_started).count()
              <<"s\n";
      if(usb_queue_) {
        std::cerr<<"TX_PRODUCER pushes="<<diag_pushes
                 <<" first_push_from_ptt_ms="<<diag_first_push_ms
                 <<" max_push_gap_ms="<<diag_max_push_gap_ms
                 <<" max_mod_ms="<<diag_max_mod_ms
                 <<" max_disk_write_ms="<<diag_max_disk_ms<<"\n";
      }
      mod.reset();
      active_=false;
   }
   was_on=on;
   const ssize_t n=read(fd,data.data(),data.size());
   if(n>0){
     last_fifo_data=std::chrono::steady_clock::now();
     std::size_t samples=0;
     for(ssize_t j=0;j<n;++j){
       if(!held_byte){previous=data[static_cast<std::size_t>(j)];held_byte=true;}
       else {const std::uint16_t word=static_cast<std::uint16_t>(previous)|
            (static_cast<std::uint16_t>(data[static_cast<std::size_t>(j)])<<8);
         pcm[samples++]=static_cast<std::int16_t>(word);held_byte=false;
       }
     }
     if(on && mod && file && samples) {
       if(pcm_file) {
         pcm_file.write(reinterpret_cast<const char*>(pcm.data()),
                        static_cast<std::streamsize>(samples*sizeof(pcm[0])));
         if(!pcm_file) {std::cerr<<"TX DIAG: error escribiendo PCM espejo\n";
           failed_=true;ptt_=false;on=false;}
       }
       for(std::size_t j=0;j<samples;++j) {
         if(pcm[j]!=0) {
           if(diag_first==std::numeric_limits<std::uint64_t>::max()) {
             diag_first=diag_samples+j;
             diag_first_wall_ms=std::chrono::duration<double,std::milli>(
                 std::chrono::steady_clock::now()-ptt_started).count();
             std::ostringstream msg;
             msg<<"TX_DIAG first_audio_pcm_ms="<<(1000.0*double(diag_first)/48000.0)
                <<" first_audio_wall_ms="<<diag_first_wall_ms
                <<(usb_queue_?" RF_TX=ENABLED":" RF_TX=DISABLED");
             cycle_log(msg.str());
           }
           diag_last=diag_samples+j;
           ++diag_nonzero;
         }
       }
       diag_samples+=samples;
        const auto mod_begin=std::chrono::steady_clock::now();
        mod->process(pcm.data(),samples,iq);
        const auto mod_end=std::chrono::steady_clock::now();
        diag_max_mod_ms=std::max(diag_max_mod_ms,
          std::chrono::duration<double,std::milli>(mod_end-mod_begin).count());
        if(usb_queue_ && !iq.empty()) {
          const auto now=std::chrono::steady_clock::now();
          if(diag_pushes==0) diag_first_push_ms=
            std::chrono::duration<double,std::milli>(now-ptt_started).count();
          else diag_max_push_gap_ms=std::max(diag_max_push_gap_ms,
            std::chrono::duration<double,std::milli>(now-diag_previous_push).count());
          diag_previous_push=now;
          ++diag_pushes;
        }
        if(usb_queue_ && !usb_queue_->push(iq)) {
         std::cerr<<"TX USB: IQ queue overflow; aborting PTT\n";
         failed_=true;ptt_=false;on=false;
       }
        const auto disk_begin=std::chrono::steady_clock::now();
        file.write(reinterpret_cast<const char*>(iq.data()),static_cast<std::streamsize>(iq.size()));
        diag_max_disk_ms=std::max(diag_max_disk_ms,
          std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-disk_begin).count());
       if(!file){std::cerr<<"TX SIM: fallo de escritura, PTT sim desactivado\n";
         failed_=true;ptt_=false;on=false;}
     }
   } else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR){
     std::cerr<<"TX SIM: fallo lectura FIFO "<<strerror(errno)<<"\n";
     failed_=true;ptt_=false;break;
   } else {
     // parec normally streams PCM (including zeros) while WSJT-X is idle.
     // A dead input must not keep an unattended long PTT active indefinitely.
     if(on && std::chrono::steady_clock::now()-last_fifo_data>std::chrono::seconds(5)) {
       std::cerr<<"TX SIM: FIFO sin datos durante 5 s; abortando TX sim\n";
       failed_=true;ptt_=false;on=false;
     }
     std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
 }
 if(mod&&file){
   mod->flush(iq);
   file.write(reinterpret_cast<const char*>(iq.data()),static_cast<std::streamsize>(iq.size()));
   if(!file) failed_=true;
 }
 if(file.is_open()){file.close();if(file.fail()) failed_=true;}
 if(pcm_file.is_open()){pcm_file.close();if(pcm_file.fail()) failed_=true;}
 active_=false;
 if(!quit_.load()) {failed_=true;ptt_=false;}
 ::close(fd);
}
