#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// RX-only USB demodulator: IQ 2 MS/s (signed interleaved I,Q)
// -> mono PCM 48 kHz (signed 16-bit), with 10-kHz LO offset.
// DDC: complex translation, CIC5 /40 (50 kHz),
// sixth-order complex low-pass at 50 kHz, back-shift, 50->48-kHz interpolation.
class UsbDemod {
 public:
  UsbDemod(double rx_offset_hz = 10000.0, double audio_center_hz = 1700.0);
  void process(const std::int8_t* iq, std::size_t bytes,
               std::vector<std::int16_t>& out);
  std::uint64_t samples_in() const { return in_count_; }
  std::uint64_t samples_out() const { return out_count_; }
 private:
  struct Biquad {
    double b0, b1, b2, a1, a2;
    double xi1 = 0, xi2 = 0, yi1 = 0, yi2 = 0;
    double xq1 = 0, xq2 = 0, yq1 = 0, yq2 = 0;
    void setup(double cutoff_hz, double sample_rate_hz, double q);
    void filter(double& i, double& q);
  } filters_[3];
  static const unsigned kDecimation = 40;
  static const unsigned kOrder = 5;
  std::uint64_t int_i_[kOrder] = {}, int_q_[kOrder] = {};
  std::uint64_t comb_i_[kOrder] = {}, comb_q_[kOrder] = {};
  unsigned decim_count_ = 0;
  double osc_i_ = 1, osc_q_ = 0, step_i_ = 1, step_q_ = 0;
  double shift_i_ = 1, shift_q_ = 0, shift_step_i_ = 1, shift_step_q_ = 0;
  std::uint64_t in_count_ = 0, out_count_ = 0, decim_index_ = 0;
  double next_audio_pos_ = 0.0, last_audio_ = 0.0;
  bool last_audio_valid_ = false;
};
