// airspy-fmradion
// Software decoder for FM broadcast radio with Airspy
//
// Copyright (C) 2015 Edouard Griffiths, F4EXB
// Copyright (C) 2019 Kenji Rikitake, JJ1BDX
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <cassert>
#include <cmath>

#include "FmDecode.h"
#include "Utility.h"

// Define this to print IF AGC level to stderr
// #define DEBUG_IF_AGC

/* ****************  class PilotPhaseLock  **************** */

// Construct phase-locked loop.
PilotPhaseLock::PilotPhaseLock(double freq, double bandwidth,
                               double minsignal) {
  /*
   * This is a type-2, 4th order phase-locked loop.
   *
   * Open-loop transfer function:
   *   G(z) = K * (z - q1) / ((z - p1) * (z - p2) * (z - 1) * (z - 1))
   *   K  = 3.788 * (bandwidth * 2 * Pi)**3
   *   q1 = exp(-0.1153 * bandwidth * 2*Pi)
   *   p1 = exp(-1.146 * bandwidth * 2*Pi)
   *   p2 = exp(-5.331 * bandwidth * 2*Pi)
   *
   * I don't understand what I'm doing; hopefully it will work.
   */

  // Set min/max locking frequencies.
  m_minfreq = (freq - bandwidth) * 2.0 * M_PI;
  m_maxfreq = (freq + bandwidth) * 2.0 * M_PI;

  // Set valid signal threshold.
  m_minsignal = minsignal;
  m_lock_delay = int(20.0 / bandwidth);
  m_lock_cnt = 0;
  m_pilot_level = 0;

  // Create 2nd order filter for I/Q representation of phase error.
  // Filter has two poles, unit DC gain.
  double p1 = exp(-1.146 * bandwidth * 2.0 * M_PI);
  double p2 = exp(-5.331 * bandwidth * 2.0 * M_PI);
  m_phasor_a1 = -p1 - p2;
  m_phasor_a2 = p1 * p2;
  m_phasor_b0 = 1 + m_phasor_a1 + m_phasor_a2;

  // Create loop filter to stabilize the loop.
  double q1 = exp(-0.1153 * bandwidth * 2.0 * M_PI);
  m_loopfilter_b0 = 0.62 * bandwidth * 2.0 * M_PI;
  m_loopfilter_b1 = -m_loopfilter_b0 * q1;

  // After the loop filter, the phase error is integrated to produce
  // the frequency. Then the frequency is integrated to produce the phase.
  // These integrators form the two remaining poles, both at z = 1.

  // Initialize frequency and phase.
  m_freq = freq * 2.0 * M_PI;
  m_phase = 0;

  m_phasor_i1 = 0;
  m_phasor_i2 = 0;
  m_phasor_q1 = 0;
  m_phasor_q2 = 0;
  m_loopfilter_x1 = 0;

  // Initialize PPS generator.
  m_pilot_periods = 0;
  m_pps_cnt = 0;
  m_sample_cnt = 0;
}

// Process samples and generate the 38kHz locked tone.
void PilotPhaseLock::process(const SampleVector &samples_in,
                             SampleVector &samples_out, bool pilot_shift) {
  unsigned int n = samples_in.size();

  samples_out.resize(n);

  bool was_locked = (m_lock_cnt >= m_lock_delay);
  m_pps_events.clear();

  if (n > 0) {
    m_pilot_level = 1000.0;
  } else {
    // n == 0
    // Do nothing when the input size is 0
    return;
  }

  for (unsigned int i = 0; i < n; i++) {

    // Generate locked pilot tone.
    Sample psin = sin(m_phase);
    Sample pcos = cos(m_phase);

    // Generate double-frequency output.
    if (pilot_shift) {
      // Use cos(2*x) to shift phase for pi/4 (90 degrees)
      // cos(2*x) = 2 * cos(x) * cos(x) - 1
      samples_out[i] = 2 * pcos * pcos - 1;
    } else {
      // Proper phase: not shifted
      // sin(2*x) = 2 * sin(x) * cos(x)
      samples_out[i] = 2 * psin * pcos;
    }

    // Multiply locked tone with input.
    Sample x = samples_in[i];
    Sample phasor_i = psin * x;
    Sample phasor_q = pcos * x;

    // Run IQ phase error through low-pass filter.
    phasor_i = m_phasor_b0 * phasor_i - m_phasor_a1 * m_phasor_i1 -
               m_phasor_a2 * m_phasor_i2;
    phasor_q = m_phasor_b0 * phasor_q - m_phasor_a1 * m_phasor_q1 -
               m_phasor_a2 * m_phasor_q2;
    m_phasor_i2 = m_phasor_i1;
    m_phasor_i1 = phasor_i;
    m_phasor_q2 = m_phasor_q1;
    m_phasor_q1 = phasor_q;

    // Convert I/Q ratio to estimate of phase error.
    // Note: maximum phase error during the locked state is +- 0.02 radian.
    // Sample phase_err = atan2(phasor_q, phasor_i);
    Sample phase_err = Utility::fast_atan2f(phasor_q, phasor_i);

    // Detect pilot level (conservative).
    m_pilot_level = std::min(m_pilot_level, phasor_i);

    // Run phase error through loop filter and update frequency estimate.
    m_freq += m_loopfilter_b0 * phase_err + m_loopfilter_b1 * m_loopfilter_x1;
    m_loopfilter_x1 = phase_err;

    // Limit frequency to allowable range.
    m_freq = std::max(m_minfreq, std::min(m_maxfreq, m_freq));

    // Update locked phase.
    m_phase += m_freq;
    if (m_phase > 2.0 * M_PI) {
      m_phase -= 2.0 * M_PI;
      m_pilot_periods++;

      // Generate pulse-per-second.
      if (m_pilot_periods == pilot_frequency) {
        m_pilot_periods = 0;
        if (was_locked) {
          struct PpsEvent ev;
          ev.pps_index = m_pps_cnt;
          ev.sample_index = m_sample_cnt + i;
          ev.block_position = double(i) / double(n);
          m_pps_events.push_back(ev);
          m_pps_cnt++;
        }
      }
    }
  }

  // Update lock status.
  if (2 * m_pilot_level > m_minsignal) {
    if (m_lock_cnt < m_lock_delay) {
      m_lock_cnt += n;
    }
  } else {
    m_lock_cnt = 0;
  }

  // Drop PPS events when pilot not locked.
  if (m_lock_cnt < m_lock_delay) {
    m_pilot_periods = 0;
    m_pps_cnt = 0;
    m_pps_events.clear();
  }

  // Update sample counter.
  m_sample_cnt += n;
}

// class FmDecoder

FmDecoder::FmDecoder(double sample_rate_demod, bool stereo, double deemphasis,
                     bool pilot_shift, unsigned int multipath_stages)
    // Initialize member fields
    : m_sample_rate_fmdemod(sample_rate_demod), m_pilot_shift(pilot_shift),
      m_enable_multipath_filter((multipath_stages > 0)),
      m_skip_multipath_filter(false),
      // Wait first 100 blocks to enable the multipath filter
      m_wait_multipath_blocks(100), m_multipath_stages(multipath_stages),
      m_stereo_enabled(stereo), m_stereo_detected(false), m_baseband_mean(0),
      m_baseband_level(0), m_if_rms(0.0)

      // Construct AudioResampler for mono and stereo channels
      ,
      m_audioresampler_mono(m_sample_rate_fmdemod, sample_rate_pcm),
      m_audioresampler_stereo(m_sample_rate_fmdemod, sample_rate_pcm)

      // Construct 19kHz pilot signal cut filter
      ,
      m_pilotcut_mono(FilterParameters::jj1bdx_48khz_fmaudio),
      m_pilotcut_stereo(FilterParameters::jj1bdx_48khz_fmaudio)

      // Construct PhaseDiscriminator
      ,
      m_phasedisc(freq_dev / m_sample_rate_fmdemod)

      // Construct PilotPhaseLock
      ,
      m_pilotpll(pilot_freq / m_sample_rate_fmdemod, // freq
                 50 / m_sample_rate_fmdemod,         // bandwidth
                 0.01)                               // minsignal (was 0.04)

      // Construct HighPassFilterIir
      // cutoff: 4.8Hz for 48kHz sampling rate
      ,
      m_dcblock_mono(0.0001), m_dcblock_stereo(0.0001)

      // Construct LowPassFilterRC for deemphasis
      // Note: sampling rate is of the FM demodulator
      ,
      m_deemph_mono((deemphasis == 0)
                        ? 1.0
                        : (deemphasis * m_sample_rate_fmdemod * 1.0e-6)),
      m_deemph_stereo((deemphasis == 0)
                          ? 1.0
                          : (deemphasis * m_sample_rate_fmdemod * 1.0e-6))

      // Construct IF AGC
      ,
      m_ifagc(1.0, 10000.0, MultipathFilter::if_target_level, 0.001)

      // Construct multipath filter
      // for 384kHz IF: 288 -> 750 microseconds (288/384000 * 1000000)
      ,
      m_multipathfilter(m_enable_multipath_filter ? m_multipath_stages : 1)

{
  // Do nothing
}

void FmDecoder::process(const IQSampleVector &samples_in, SampleVector &audio) {

  // If no sampled baseband signal comes out,
  // terminate and wait for next block,
  if (samples_in.size() == 0) {
    audio.resize(0);
    return;
  }

  // Measure IF RMS level.
  m_if_rms = Utility::rms_level_approx(samples_in);

  // Perform IF AGC.
  m_ifagc.process(samples_in, m_samples_in_after_agc);

#ifdef DEBUG_IF_AGC
  // Measure IF RMS level for checking how IF AGC works.
  float if_agc_rms = Utility::rms_level_approx(m_samples_in_after_agc);
  fprintf(stderr, "if_agc_rms = %.9g\n", if_agc_rms);
#endif

  if (m_wait_multipath_blocks > 0) {
    m_wait_multipath_blocks--;
    // No multipath filter applied.
    m_samples_in_filtered = std::move(m_samples_in_after_agc);
  } else {
    if (m_enable_multipath_filter && !m_skip_multipath_filter) {
      // Apply multipath filter.
      m_multipathfilter.process(m_samples_in_after_agc, m_samples_in_filtered);
      double filter_error = m_multipathfilter.get_error();
      bool abnormal_error = !std::isfinite(filter_error);
      float mf_reference_level = m_multipathfilter.get_reference_level();
      bool reference_level_error = (std::fabs(mf_reference_level) < 0.01);
      // Reset the filter coefficients
      // if the error evaluation becomes invalid.
      if (!m_skip_multipath_filter &&
          (abnormal_error || reference_level_error)) {
        m_multipathfilter.initialize_coefficients();
        // fprintf(stderr, "Reset Multipath Filter coefficients\n");
        // Discard the invalid filter output, and
        // use the no-filter input after resetting the filter.
        m_samples_in_filtered = std::move(m_samples_in_after_agc);
      }
    } else {
      // No multipath filter applied.
      m_samples_in_filtered = std::move(m_samples_in_after_agc);
    }
  }

  // Demodulate FM to MPX signal.
  m_phasedisc.process(m_samples_in_filtered, m_buf_decoded);

  // If no downsampled baseband signal comes out,
  // terminate and wait for next block,
  size_t decoded_size = m_buf_decoded.size();
  if (decoded_size == 0) {
    audio.resize(0);
    return;
  }

  // Convert decoded data to baseband data
  m_buf_baseband.resize(decoded_size);
  volk_32f_convert_64f(m_buf_baseband.data(), m_buf_decoded.data(),
                       decoded_size);

  // Measure baseband level.
  float baseband_mean, baseband_rms;
  Utility::samples_mean_rms(m_buf_decoded, baseband_mean, baseband_rms);
  m_baseband_mean = 0.95 * m_baseband_mean + 0.05 * baseband_mean;
  m_baseband_level = 0.95 * m_baseband_level + 0.05 * baseband_rms;

  // The following function must be executed anyway
  // even if the mono audio resampler output does not come out.
  if (m_stereo_enabled) {
    // Lock on stereo pilot,
    // and remove locked 19kHz tone from the composite signal.
    m_pilotpll.process(m_buf_baseband, m_buf_rawstereo, m_pilot_shift);

    // Force-set this flag to true to measure stereo PLL phase noise
    m_stereo_detected = true;
    // Use locked flag for the normal use
    // m_stereo_detected = m_pilotpll.locked();

    // Demodulate stereo signal.
    demod_stereo(m_buf_baseband, m_buf_rawstereo);

    // Deemphasize the stereo (L-R) signal if not for QMM.
    if (!m_pilot_shift) {
      m_deemph_stereo.process_inplace(m_buf_rawstereo);
    }

    // Downsample.
    // NOTE: This MUST be done even if no stereo signal is detected yet,
    // because the downsamplers for mono and stereo signal must be
    // kept in sync.
    m_audioresampler_stereo.process(m_buf_rawstereo, m_buf_stereo_firstout);
  }

  // Deemphasize the mono audio signal.
  m_deemph_mono.process_inplace(m_buf_baseband);

  // Extract mono audio signal.
  m_audioresampler_mono.process(m_buf_baseband, m_buf_mono_firstout);
  // If no mono audio signal comes out, terminate and wait for next block,
  if (m_buf_mono_firstout.size() == 0) {
    audio.resize(0);
    return;
  }
  // Filter out mono 19kHz pilot signal.
  m_pilotcut_mono.process(m_buf_mono_firstout, m_buf_mono);
  // DC blocking
  m_dcblock_mono.process_inplace(m_buf_mono);

  if (m_stereo_enabled) {
    // Filter out mono 19kHz pilot signal.
    m_pilotcut_stereo.process(m_buf_stereo_firstout, m_buf_stereo);
    // DC blocking
    m_dcblock_stereo.process_inplace(m_buf_stereo);

    if (m_stereo_detected) {
      if (m_pilot_shift) {
        // Duplicate L-R shifted output in left/right channels.
        mono_to_left_right(m_buf_stereo, audio);
      } else {
        // Extract left/right channels from (L+R) / (L-R) signals.
        stereo_to_left_right(m_buf_mono, m_buf_stereo, audio);
      }
    } else {
      if (m_pilot_shift) {
        // Fill zero output in left/right channels.
        zero_to_left_right(m_buf_stereo, audio);
      } else {
        // Duplicate mono signal in left/right channels.
        mono_to_left_right(m_buf_mono, audio);
      }
    }
  } else {
    // Just return mono channel.
    audio = std::move(m_buf_mono);
  }
}

// Demodulate stereo L-R signal.
inline void FmDecoder::demod_stereo(const SampleVector &samples_baseband,
                                    SampleVector &samples_rawstereo) {
  // Multiply the baseband signal with the double-frequency pilot,
  // and multiply by 2.00 to get the full amplitude.

  unsigned int n = samples_baseband.size();
  assert(n == samples_rawstereo.size());

  for (unsigned int i = 0; i < n; i++) {
    samples_rawstereo[i] *= 2.00 * samples_baseband[i];
  }
}

// Duplicate mono signal in left/right channels.
inline void FmDecoder::mono_to_left_right(const SampleVector &samples_mono,
                                          SampleVector &audio) {
  unsigned int n = samples_mono.size();

  audio.resize(2 * n);
  for (unsigned int i = 0; i < n; i++) {
    Sample m = samples_mono[i];
    audio[2 * i] = m;
    audio[2 * i + 1] = m;
  }
}

// Extract left/right channels from (L+R) / (L-R) signals.
inline void FmDecoder::stereo_to_left_right(const SampleVector &samples_mono,
                                            const SampleVector &samples_stereo,
                                            SampleVector &audio) {
  unsigned int n = samples_mono.size();
  assert(n == samples_stereo.size());

  audio.resize(2 * n);
  for (unsigned int i = 0; i < n; i++) {
    Sample m = samples_mono[i];
    // L-R singal is boosted by 1.017
    // for better separation (suggested by Teruhiko Hayashi)
    Sample s = 1.017 * samples_stereo[i];
    audio[2 * i] = m + s;
    audio[2 * i + 1] = m - s;
  }
}

// Fill zero signal in left/right channels.
// (samples_mono used for the size determination only)
inline void FmDecoder::zero_to_left_right(const SampleVector &samples_mono,
                                          SampleVector &audio) {
  unsigned int n = samples_mono.size();

  audio.resize(2 * n);
  for (unsigned int i = 0; i < n; i++) {
    audio[2 * i] = 0.0;
    audio[2 * i + 1] = 0.0;
  }
}

/* end */
