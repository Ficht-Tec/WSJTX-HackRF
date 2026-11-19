#include "rigctl_server_cycle.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
const char* ok="RPRT 0\n";
const char* bad="RPRT -1\n";
const char* unsupported="RPRT -4\n";
const std::uint64_t kMinDial=1010000, kMaxDial=6000000000ULL;
std::string trim(const std::string& s) {
  const std::size_t b=s.find_first_not_of(" \t\r\n");
  return b==std::string::npos ? "" : s.substr(b,s.find_last_not_of(" \t\r\n")-b+1);
}
bool number(const std::string& s,std::uint64_t& value) {
  if(s.empty() || s[0]=='-' || s[0]=='+') return false;
  try {
    std::size_t consumed=0;
    double d=std::stod(s,&consumed);
    if(consumed!=s.size() || !std::isfinite(d) || d<0 || d>6000000000.0) return false;
    value=static_cast<std::uint64_t>(std::llround(d)); return true;
  } catch(...) {return false;}
}
bool integer(const std::string& s,int& value) {
  try { std::size_t used=0; int v=std::stoi(s,&used); if(used!=s.size()) return false; value=v; return true; }
  catch(...) {return false;}
}
std::vector<std::string> words(const std::string& s) {
  std::istringstream in(s); std::string token; std::vector<std::string> out;
  while(in>>token) out.push_back(token);
  return out;
}
std::string strip_vfo(const std::vector<std::string>& fields,std::size_t idx) {
  return idx<fields.size() ? fields[idx] : "";
}
// Hamlib NET rigctl protocol v0: capabilities used by netrigctl_open().
// Only RX USB/PKTUSB and VFO A are advertised. No TX RF capabilities.
// The exact ordered record format is required; an RPRT status is NOT used here.
std::string dump_state_rx_only() {
  return
    "0\n"                // rigctld protocol version 0
    "1\n"                // model identifier (metadata only)
    "0\n"                // legacy ITU region
    "1010000 6000000000 0x804 -1 -1 0x1 0x0\n" // RX, USB + PKTUSB, VFOA
    "0 0 0 0 0 0 0\n" // end RX ranges
    "0 0 0 0 0 0 0\n" // NO TX ranges
    "0x804 1\n"        // tuning step 1 Hz, both USB modes
    "0 0\n"            // end steps
    "0x804 3000\n"     // passband 3000 Hz
    "0 0\n"            // end filters
    "0\n"              // max RIT
    "0\n"              // max XIT
    "0\n"              // max IF shift
    "0\n"              // announces
    "\n"               // preamps (none)
    "\n"               // attenuators (none)
    "0x0\n"            // get functions
    "0x0\n"            // set functions
    "0x0\n"            // get levels
    "0x0\n"            // set levels
    "0x0\n"            // get parameters
    "0x0\n";           // set parameters
}
void nonblocking(int fd) {
  const int flags=fcntl(fd,F_GETFL,0);
  if(flags>=0) fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
bool send_text(int fd,const std::string& str) {
  std::size_t offset=0;
  while(offset<str.size()) {
    const ssize_t n=send(fd,str.data()+offset,str.size()-offset,MSG_NOSIGNAL);
    if(n>0) {offset+=static_cast<std::size_t>(n); continue;}
    if(n<0 && errno==EINTR) continue;
    if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
      pollfd p={fd,POLLOUT,0}; if(poll(&p,1,1000)>0) continue;
    }
    return false;
  }
  return true;
}
}
RadioControl::RadioControl(const RadioSettings& initial):desired_(initial),actual_(initial) {}
RadioSettings RadioControl::current() {
  std::lock_guard<std::mutex> lock(mutex_);return actual_;
}
bool RadioControl::request_frequency(std::uint64_t hz) {return request(FREQ,hz);}
bool RadioControl::request_lna(int g) {return request(LNA,static_cast<unsigned>(g));}
bool RadioControl::request_vga(int g) {return request(VGA,static_cast<unsigned>(g));}
bool RadioControl::request_amp(int g) {return request(AMP,static_cast<unsigned>(g));}
bool RadioControl::stage_rx_lna(int g) {return stage_rx(LNA,g);}
bool RadioControl::stage_rx_vga(int g) {return stage_rx(VGA,g);}
RadioSettings RadioControl::staged_rx() {
  std::lock_guard<std::mutex> lock(mutex_);return desired_;
}
bool RadioControl::stage_rx(Field what,int gain) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(closing_) return false;
  if(what==LNA) desired_.lna=gain;
  else if(what==VGA) desired_.vga=gain;
  else return false;
  if(issued_==completed_ && desired_.lna==actual_.lna && desired_.vga==actual_.vga) return true;
  ++issued_; // main RX thread will apply after PTT OFF, never in RF TX
  return true;
}

bool RadioControl::stage_tx_vga(int gain) {
  if(gain<0 || gain>47) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if(closing_) return false;
  staged_tx_vga_=gain;
  tx_vga_update_pending_=true; // main hardware-owner thread applies in TX
  return true;
}
int RadioControl::staged_tx_vga() {
  std::lock_guard<std::mutex> lock(mutex_);
  return staged_tx_vga_;
}
int RadioControl::applied_tx_vga() {
  std::lock_guard<std::mutex> lock(mutex_);
  return applied_tx_vga_;
}
bool RadioControl::take_tx_vga_request(int& gain) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(!tx_vga_update_pending_) return false;
  gain=staged_tx_vga_;
  tx_vga_update_pending_=false;
  return true;
}
void RadioControl::confirm_tx_vga(int gain,bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(success) applied_tx_vga_=gain;
}
void RadioControl::clear_tx_vga_applied() {
  std::lock_guard<std::mutex> lock(mutex_);
  applied_tx_vga_=-1;
}

bool RadioControl::request(Field what,std::uint64_t val) {
  std::unique_lock<std::mutex> lock(mutex_);
  if(closing_) return false;
  RadioSettings target=desired_;
  if(what==FREQ) target.dial_hz=val;
  if(what==LNA) target.lna=static_cast<int>(val);
  if(what==VGA) target.vga=static_cast<int>(val);
  if(what==AMP) target.amp=static_cast<int>(val);
  if(target.dial_hz==actual_.dial_hz && target.lna==actual_.lna && target.vga==actual_.vga && target.amp==actual_.amp)
    return true;
  desired_=target;
  const std::uint64_t serial=++issued_;
  bool done=cv_.wait_for(lock,std::chrono::seconds(4),[&]{return completed_>=serial || closing_;});
  return done && !closing_ && completed_>=serial && result_;
}
bool RadioControl::take_pending(RadioSettings& target,std::uint64_t& serial) {
  std::lock_guard<std::mutex> lock(mutex_);
  if(issued_==completed_) return false;
  target=desired_;serial=issued_;return true;
}
void RadioControl::complete(std::uint64_t serial,bool success,const RadioSettings& current) {
  std::lock_guard<std::mutex> lock(mutex_);
  actual_=current;
  if(!success) desired_=current;
  completed_=serial;
  result_=success;
  cv_.notify_all();
}
void RadioControl::close() {
  std::lock_guard<std::mutex> lock(mutex_); closing_=true;cv_.notify_all();
}
RigctlCycleServer::RigctlCycleServer(RadioControl& control,PttCycle& cycle,bool rf_enabled):rf_enabled_(rf_enabled),control_(control),cycle_(cycle) {}
RigctlCycleServer::~RigctlCycleServer() { stop(); }
bool RigctlCycleServer::start(unsigned port) {
  if(!port || port>65535) return false;
  listen_fd_=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
  if(listen_fd_<0) return false;
  int yes=1;setsockopt(listen_fd_,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  sockaddr_in addr={};addr.sin_family=AF_INET;addr.sin_port=htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK); // localhost only
  if(bind(listen_fd_,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr))<0 ||
     listen(listen_fd_,4)<0) {
    std::cerr<<"CAT: no puedo abrir 127.0.0.1:"<<port<<": "<<strerror(errno)<<"\n";
    ::close(listen_fd_);listen_fd_=-1;return false;
  }
  nonblocking(listen_fd_);
  thread_=std::thread(&RigctlCycleServer::loop,this);
  std::cerr<<"CAT rigctl ("<<(rf_enabled_?"TX RF":"TX SIM")<<") en 127.0.0.1:"<<port<<"\n";
  return true;
}
void RigctlCycleServer::stop() {
  quit_=true;
  control_.close();
  cycle_.close();
  if(thread_.joinable()) thread_.join();
  if(listen_fd_>=0) {::close(listen_fd_);listen_fd_=-1;}
}

std::string RigctlCycleServer::respond(const std::string& line,bool& disconnect) {
  const std::vector<std::string> args=words(trim(line));
  if(args.empty()) return "";
  std::string cmd=args[0];
  if(cmd=="q" || cmd=="Q" || cmd=="\\quit") {disconnect=true;return "";}
  const RadioSettings s=control_.current();
  if(cmd=="f" || cmd=="\\get_freq") return std::to_string(s.dial_hz)+"\n";
  if(cmd=="F" || cmd=="\\set_freq") {
    std::uint64_t hz=0;
    if(args.size()<2 || !number(args.back(),hz) || hz<kMinDial || hz>kMaxDial) return bad;
    return control_.request_frequency(hz)?ok:bad;
  }
  if(cmd=="m" || cmd=="\\get_mode") return "USB\n3000\n";
  if(cmd=="M" || cmd=="\\set_mode") {
    std::size_t idx=1;
    if(args.size()>2 && (args[1]=="VFOA" || args[1]=="currVFO")) ++idx;
    if(args.size()<=idx || (args[idx]!="USB" && args[idx]!="PKTUSB")) return unsupported;
    return ok;
  }
  if(cmd=="v" || cmd=="\\get_vfo") return "VFOA\n";
  if(cmd=="V" || cmd=="\\set_vfo") {
    return args.size()==2 && (args[1]=="VFOA" || args[1]=="currVFO")?ok:unsupported;
  }
  if(cmd=="t" || cmd=="\\get_ptt") return cycle_.active()?"1\n":"0\n";
  if(cmd=="T" || cmd=="\\set_ptt") {
    // rigctld PTT: 0=RX, 1=TX, 2=TX microphone, 3=TX data.
    // Nonzero PTT requests a TX cycle; the main thread selects SIM or RF.
    if(args.size()!=2 || (args[1]!="0" && args[1]!="1" &&
                          args[1]!="2" && args[1]!="3")) return unsupported;
    const bool on=(args[1]!="0");
    const auto start=std::chrono::steady_clock::now();
    const bool result=cycle_.request(on);
    std::ostringstream msg;
    msg<<"CAT: PTT "<<(on?"ON":"OFF")<<" (T "<<args[1]
       <<") "<<(result?"ACK":"REJECT")<<" response_ms="
       <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()
       <<" RF_TX="<<(rf_enabled_?"ARMED":"DISABLED");
    cycle_log(msg.str());
    return result?ok:bad;
  }
  if(cmd=="\\chk_vfo") return "0\n";
  if(cmd=="\\dump_state") return dump_state_rx_only();
  if(cmd=="\\set_vfo_opt") return args.size()==2 && args[1]=="0"?ok:unsupported;
  if(cmd=="\\get_powerstat") return "1\n";
  if(cmd=="\\get_info" || cmd=="_") return "Bridge-HackRF v0.4b RX state diagnostics (NO RF)\n";
  // Private extensions: not part of rigctld's standard protocol.
  if(cmd=="\\set_lna" || cmd=="\\set_vga") {
    int n=0;
    if(args.size()!=2 || !integer(args[1],n)) return bad;
    if(cmd=="\\set_lna") return n>=0 && n<=40 && n%8==0 && (cycle_.active()?control_.stage_rx_lna(n):control_.request_lna(n))?ok:bad;
    return n>=0 && n<=62 && n%2==0 && (cycle_.active()?control_.stage_rx_vga(n):control_.request_vga(n))?ok:bad;
  }
  if(cmd=="\\get_lna") return std::to_string(s.lna)+"\n";
  if(cmd=="\\get_vga") return std::to_string(s.vga)+"\n";
  if(cmd=="\\get_amp") return std::to_string(s.amp)+"\n";
  if(cmd=="\\set_amp") {
    int n=0;
    if(args.size()!=2 || !integer(args[1],n) || (n!=0 && n!=1)) return bad;
    if(cycle_.active()) return bad; // no amplifier change during TX_SIM
    return control_.request_amp(n)?ok:bad;
  }
  // Store TX gain in RX or TX. During TX the hardware-owner thread applies
  // the latest requested value without blocking the USB callback.
  if(cmd=="\\set_tx_vga") {
    int gain=0;
    if(args.size()!=2 || !integer(args[1],gain)) return bad;
    return control_.stage_tx_vga(gain)?ok:bad;
  }
  if(cmd=="\\get_tx_vga") return std::to_string(control_.staged_tx_vga())+"\n";
  if(cmd=="\\status") {
    const auto staged=control_.staged_rx();
    return "dial="+std::to_string(s.dial_hz)+" lna="+std::to_string(s.lna)+
           " vga="+std::to_string(s.vga)+" lna_stage="+std::to_string(staged.lna)+
           " vga_stage="+std::to_string(staged.vga)+" amp="+std::to_string(s.amp)+" ptt_sim="+std::to_string(cycle_.active()?1:0)+
               " state="+cycle_.state()+" tx_vga_stage="+std::to_string(control_.staged_tx_vga())+
               " tx_vga_applied="+std::to_string(control_.applied_tx_vga())+
               " rf_tx="+(rf_enabled_?(cycle_.active()?"ACTIVE":"ARMED"):"DISABLED")+"\n";
  }
  return unsupported;
}
void RigctlCycleServer::loop() {
  struct Client {int fd;std::string pending;};
  std::vector<Client> clients;
  while(!quit_.load()) {
    std::vector<pollfd> fds;
    fds.push_back(pollfd{listen_fd_,POLLIN,0});
    for(const auto& c:clients) fds.push_back(pollfd{c.fd,POLLIN,0});
    const int ret=poll(fds.data(),static_cast<nfds_t>(fds.size()),100);
    if(ret<0) {if(errno==EINTR) continue;break;}
    if(ret>0 && (fds[0].revents&POLLIN)) {
      int fd=accept4(listen_fd_,nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
      if(fd>=0) {
        if(clients.size()<4) clients.push_back(Client{fd,""});
        else ::close(fd);
      }
    }
    for(std::size_t i=clients.size();i>0;--i) {
      const std::size_t ci=i-1;
      // poll index is based on the original clients ordering.
      const short event=(ci+1<fds.size())?fds[ci+1].revents:0;
      if(!(event&(POLLIN|POLLHUP|POLLERR|POLLNVAL))) continue;
      Client& c=clients[ci];
      bool dead=false;
      if(event&POLLIN) {
        char buf[512];const ssize_t n=recv(c.fd,buf,sizeof(buf),0);
        if(n<=0) dead=true;
        else {
          c.pending.append(buf,static_cast<std::size_t>(n));
          if(c.pending.size()>4096) dead=true;
          while(!dead) {
            const std::size_t pos=c.pending.find('\n');
            if(pos==std::string::npos) break;
            const std::string line=c.pending.substr(0,pos);
            c.pending.erase(0,pos+1);
            bool disconnect=false;
            const std::string result=respond(line,disconnect);
            if((!result.empty() && !send_text(c.fd,result)) || disconnect) dead=true;
          }
        }
      } else dead=true;
      if(dead) {::close(c.fd);clients.erase(clients.begin()+ci);}
    }
  }
  for(const auto& c:clients) ::close(c.fd);
}
