#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <complex>
#include <vector>

// OFF-AIR USB modulator. Input: real PCM s16le, 48 kHz.
// Output: complex signed 8-bit interleaved IQ at 2 MS/s.
// The IQ carrier is dial + audio relative to a *hypothetical* LO at dial-10 kHz.
// No HackRF TX API or hardware code is included here.
class UsbTxModulator {
public:
  UsbTxModulator();
  void process(const std::int16_t* pcm, std::size_t n,
               std::vector<std::int8_t>& iq_out);
  // Finalise one offline recording with the tail of the FIR/resampler.
  void flush(std::vector<std::int8_t>& iq_out);
  std::uint64_t audio_samples() const {return audio_count_;}
  std::uint64_t iq_samples() const {return iq_count_;}
private:
  static const unsigned kFilter = 257;
  static const unsigned kHalf = (kFilter-1)/2;
  static const unsigned kInterpTaps = 32;
  static const unsigned kInterpPhaseCount = 125;
  std::array<double,kFilter> tap_real_{};
  std::array<double,kFilter> tap_imag_{};
  std::array<double,kFilter> audio_{};
  std::array<std::complex<double>,64> analytic_{};
  std::array<std::array<double,kInterpTaps>,kInterpPhaseCount> interp_{};
  std::uint64_t audio_count_=0;
  std::uint64_t iq_count_=0;
  std::complex<double> carrier_{1.0,0.0};
  std::complex<double> carrier_step_;
  void consume(std::int16_t sample,std::vector<std::int8_t>& iq_out);
};
