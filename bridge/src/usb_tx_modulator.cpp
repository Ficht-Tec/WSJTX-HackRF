#include "usb_tx_modulator.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {
const double pi=3.141592653589793238462643383279502884;
const double fs=48000.0;
const double fc_lo=75.0, fc_hi=3500.0;
const double rf_offset=10000.0; // hypothetical TX center = dial-10 kHz
const double iq_rate=2000000.0;
const double iq_peak=24.0; // conservative -14.5 dBFS for a full-scale single tone
inline double sinc(double x) {
  if (std::abs(x)<1e-10) return 1.0;
  return std::sin(pi*x)/(pi*x);
}
inline std::int8_t quantize(double x) {
  // Clamp to the signed 8-bit IQ range; the 24/127 scaling prevents clipping
  // with a single full-scale voice/data tone.
  x=std::max(-127.0,std::min(127.0,x));
  return static_cast<std::int8_t>(std::lrint(x));
}
}

UsbTxModulator::UsbTxModulator() {
  // Analytic band-pass impulse response: positive audio only, 75..3500 Hz.
  // Windowed length-257 complex FIR eliminates mirror (LSB) and filters DC.
  for(int k=-static_cast<int>(kHalf);k<=static_cast<int>(kHalf);++k) {
    const unsigned idx=static_cast<unsigned>(k+static_cast<int>(kHalf));
    const double w=0.42+0.5*std::cos(pi*k/kHalf)+0.08*std::cos(2*pi*k/kHalf);
    if(k==0) {
      tap_real_[idx]=2.0*(fc_hi-fc_lo)/fs*w;
      tap_imag_[idx]=0.0;
    } else {
      tap_real_[idx]=(std::sin(2*pi*fc_hi*k/fs)-std::sin(2*pi*fc_lo*k/fs))/(pi*k)*w;
      tap_imag_[idx]=(std::cos(2*pi*fc_lo*k/fs)-std::cos(2*pi*fc_hi*k/fs))/(pi*k)*w;
    }
  }
  // 125 fractional phases; exact rational 48k -> 2M = 125/3.
  // 32-tap fractional-delay interpolation with Blackman smoothing.
  for(unsigned ph=0; ph<kInterpPhaseCount; ++ph) {
    const double fraction=double(ph)/kInterpPhaseCount;
    double total=0.0;
    for(unsigned tap=0; tap<kInterpTaps; ++tap) {
      const double x=fraction-(static_cast<int>(tap)-15);
      const double w=(std::abs(x)<16.0) ?
        (0.42+0.5*std::cos(pi*x/16.0)+0.08*std::cos(2*pi*x/16.0)):0.0;
      interp_[ph][tap]=sinc(x)*w;
      total+=interp_[ph][tap];
    }
    for(unsigned tap=0; tap<kInterpTaps; ++tap) interp_[ph][tap]/=total;
  }
  const double phase=2*pi*rf_offset/iq_rate;
  carrier_step_={std::cos(phase),std::sin(phase)};
}

void UsbTxModulator::consume(std::int16_t sample,std::vector<std::int8_t>& out) {
  const std::uint64_t n=audio_count_++;
  audio_[n%kFilter]=static_cast<double>(sample)/32768.0;
  double re=0.0,im=0.0;
  for(unsigned k=0;k<kFilter;++k) {
    if(n<k) break;
    const double x=audio_[(n-k)%kFilter];
    re+=tap_real_[k]*x;
    im+=tap_imag_[k]*x;
  }
  analytic_[n%analytic_.size()]={re,im};
  // Emit only after future samples are available for the interpolation kernel.
  // No artificial timing padding: the initial FIR delay is kept in the file.
  while((iq_count_*3ULL)/125ULL+17ULL<=n) {
    const std::uint64_t numerator=iq_count_*3ULL;
    const std::uint64_t base=numerator/125ULL;
    const unsigned phase=static_cast<unsigned>(numerator%125ULL);
    std::complex<double> s(0.0,0.0);
    for(unsigned j=0;j<kInterpTaps;++j) {
      const std::int64_t idx=static_cast<std::int64_t>(base)+static_cast<int>(j)-15;
      if(idx<0) continue;
      s+=analytic_[static_cast<std::size_t>(idx)%analytic_.size()]*interp_[phase][j];
    }
    const std::complex<double> mixed=s*carrier_;
    out.push_back(quantize(mixed.real()*iq_peak));
    out.push_back(quantize(mixed.imag()*iq_peak));
    carrier_*=carrier_step_;
    if((iq_count_&16383ULL)==0) carrier_/=std::abs(carrier_);
    ++iq_count_;
  }
}

void UsbTxModulator::process(const std::int16_t* pcm,std::size_t n,
                             std::vector<std::int8_t>& out) {
  out.clear();
  if(!pcm || !n) return;
  out.reserve(n*84+1024);
  for(std::size_t i=0;i<n;++i) consume(pcm[i],out);
}
void UsbTxModulator::flush(std::vector<std::int8_t>& out) {
  out.clear();
  // Pad 17 input samples only to release the fractional interpolator tail.
  // Analytic filter has its own causal group delay.
  for(unsigned i=0;i<17;++i) consume(0,out);
}
