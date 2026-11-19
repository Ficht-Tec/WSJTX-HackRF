#include "usb_demod.hpp"
#include <cmath>
#include <climits>

namespace {
const double kPi = 3.14159265358979323846;
const double kIqRate = 2000000.0;
const double kStageRate = 50000.0;
const double kOutputRate = 48000.0;
const double kCicGain = 102400000.0; // 40 ^ 5
void normalize(double& i, double& q) {
  const double r = std::sqrt(i*i+q*q);
  if (r > 0) { i /= r; q /= r; }
}
}

void UsbDemod::Biquad::setup(double cutoff, double fs, double q) {
  const double w = 2*kPi*cutoff/fs;
  const double c = std::cos(w), alpha = std::sin(w)/(2*q);
  const double inv_a0 = 1/(1+alpha);
  b0 = ((1-c)/2)*inv_a0;
  b1 = (1-c)*inv_a0;
  b2 = b0;
  a1 = (-2*c)*inv_a0;
  a2 = (1-alpha)*inv_a0;
}
void UsbDemod::Biquad::filter(double& i, double& q) {
  const double oi = b0*i+b1*xi1+b2*xi2-a1*yi1-a2*yi2;
  const double oq = b0*q+b1*xq1+b2*xq2-a1*yq1-a2*yq2;
  xi2=xi1; xi1=i; yi2=yi1; yi1=oi;
  xq2=xq1; xq1=q; yq2=yq1; yq1=oq;
  i=oi; q=oq;
}

UsbDemod::UsbDemod(double rx_offset_hz, double audio_center_hz) {
  // Tune the hardware to (USB dial - rx_offset_hz).
  // The FFT/audio band is then entirely away from the DC spur.
  const double w = -2*kPi*(rx_offset_hz+audio_center_hz)/kIqRate;
  step_i_ = std::cos(w); step_q_ = std::sin(w);
  const double back = 2*kPi*audio_center_hz/kStageRate;
  shift_step_i_ = std::cos(back); shift_step_q_ = std::sin(back);
  // Butterworth order 6 implemented as three 2-pole sections.
  const double qs[3] = {0.5176380902, 0.7071067812, 1.9318516526};
  for (unsigned i=0;i<3;++i) filters_[i].setup(2100.0,kStageRate,qs[i]);
}

void UsbDemod::process(const std::int8_t* iq, std::size_t bytes,
                       std::vector<std::int16_t>& out) {
  if (!iq || (bytes&1)) return;
  out.clear();
  out.reserve(bytes/80+4);
  for (std::size_t p=0; p<bytes; p+=2) {
    const double raw_i = iq[p], raw_q = iq[p+1];
    const std::int64_t mixed_i = static_cast<std::int64_t>(std::llround(raw_i*osc_i_-raw_q*osc_q_));
    const std::int64_t mixed_q = static_cast<std::int64_t>(std::llround(raw_i*osc_q_+raw_q*osc_i_));
    const double ni=osc_i_*step_i_-osc_q_*step_q_;
    const double nq=osc_q_*step_i_+osc_i_*step_q_;
    osc_i_=ni; osc_q_=nq;
    ++in_count_;
    if ((in_count_ & 4095u)==0) normalize(osc_i_,osc_q_);

    // CIC5 integrators with deliberately modular unsigned 64-bit arithmetic.
    // Never use signed 64-bit accumulation here (signed overflow is undefined).
    std::uint64_t ui=static_cast<std::uint64_t>(mixed_i);
    std::uint64_t uq=static_cast<std::uint64_t>(mixed_q);
    for (unsigned s=0;s<kOrder;++s) {
      ui = (int_i_[s] += ui);
      uq = (int_q_[s] += uq);
    }
    if (++decim_count_ < kDecimation) continue;
    decim_count_=0;
    for (unsigned s=0;s<kOrder;++s) {
      const std::uint64_t pi=comb_i_[s], pq=comb_q_[s];
      comb_i_[s]=ui; comb_q_[s]=uq;
      ui-=pi; uq-=pq;
    }
    // After five comb differences, magnitude is bounded far below INT64_MAX.
    double fi=static_cast<double>(static_cast<std::int64_t>(ui))/kCicGain;
    double fq=static_cast<double>(static_cast<std::int64_t>(uq))/kCicGain;
    for (unsigned s=0;s<3;++s) filters_[s].filter(fi,fq);
    // Shift the selected analytic USB passband back to its audio frequency.
    const double mono = fi*shift_i_-fq*shift_q_;
    const double bi=shift_i_*shift_step_i_-shift_q_*shift_step_q_;
    const double bq=shift_q_*shift_step_i_+shift_i_*shift_step_q_;
    shift_i_=bi; shift_q_=bq;
    ++decim_index_;
    if ((decim_index_&4095u)==0) normalize(shift_i_,shift_q_);

    const double n = static_cast<double>(decim_index_-1);
    if (!last_audio_valid_) {
      last_audio_=mono;
      last_audio_valid_=true;
    }
    while (next_audio_pos_ <= n+1e-9) {
      const double mix = (n == 0) ? mono :
          last_audio_+(mono-last_audio_)*(next_audio_pos_-(n-1));
      const double normalized = mix / 128.0 * 0.75;
      double pcm = normalized*32767.0;
      if (pcm>32767.0) pcm=32767.0;
      if (pcm< -32768.0) pcm=-32768.0;
      out.push_back(static_cast<std::int16_t>(std::lrint(pcm)));
      ++out_count_;
      next_audio_pos_ += kStageRate/kOutputRate;
    }
    last_audio_=mono;
  }
}
