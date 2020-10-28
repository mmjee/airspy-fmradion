// airspy-fmradion
// Software decoder for FM broadcast radio with Airspy
//
// Copyright (C) 2015 Edouard Griffiths, F4EXB
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

#ifndef SOFTFM_AUDIOOUTPUT_H
#define SOFTFM_AUDIOOUTPUT_H

#include <cstdint>
#include <cstdio>
#include <string>

#include "SoftFM.h"

#include "portaudio.h"

/** Base class for writing audio data to file or playback. */
class AudioOutput {
public:
  /** Destructor. */
  virtual ~AudioOutput() {}

  /**
   * Write audio data.
   *
   * Return true on success.
   * Return false if an error occurs.
   */
  virtual bool write(const SampleVector &samples) = 0;

  // Set type conversion function of samples.
  void SetConvertFunction(void (*converter)(const SampleVector &,
                                            std::vector<std::uint8_t> &));

  /** Encode a list of samples as signed 16-bit little-endian integers. */
  static void samplesToInt16(const SampleVector &samples,
                             std::vector<std::uint8_t> &bytes);

  /** Encode a list of samples as signed 32-bit little-endian floats. */
  static void samplesToFloat32(const SampleVector &samples,
                               std::vector<std::uint8_t> &bytes);

  /** Return the last error, or return an empty string if there is no error. */
  std::string error() {
    std::string ret(m_error);
    m_error.clear();
    return ret;
  }

  /** Return true if the stream is OK, return false if there is an error. */
  operator bool() const { return (!m_zombie) && m_error.empty(); }

  const std::string get_device_name() { return m_device_name; }

protected:
  /** Constructor. */
  AudioOutput() : m_zombie(false), m_converter(samplesToInt16) {}

  std::string m_error;
  bool m_zombie;
  void (*m_converter)(const SampleVector &, std::vector<std::uint8_t> &);
  std::string m_device_name;

private:
  AudioOutput(const AudioOutput &);            // no copy constructor
  AudioOutput &operator=(const AudioOutput &); // no assignment operator
};

/** Write audio data as raw signed 16-bit little-endian data. */
class RawAudioOutput : public AudioOutput {
public:
  /**
   * Construct raw audio writer.
   *
   * filename :: file name (including path) or "-" to write to stdout
   */
  RawAudioOutput(const std::string &filename);

  virtual ~RawAudioOutput() override;
  virtual bool write(const SampleVector &samples) override;

private:
  int m_fd;
  std::vector<std::uint8_t> m_bytebuf;
};

/** Write audio data as raw 32-bit float little-endian data. */
class FloatAudioOutput : public AudioOutput {
public:
  /**
   * Construct raw audio writer.
   *
   * filename :: file name (including path) or "-" to write to stdout
   */
  FloatAudioOutput(const std::string &filename);

  virtual ~FloatAudioOutput() override;
  virtual bool write(const SampleVector &samples) override;

private:
  int m_fd;
  std::vector<std::uint8_t> m_bytebuf;
};

/** Write audio data as .WAV file. */
class WavAudioOutput : public AudioOutput {
public:
  /**
   * Construct .WAV writer.
   *
   * filename     :: file name (including path) or "-" to write to stdout
   * samplerate   :: audio sample rate in Hz
   * stereo       :: true if the output stream contains stereo data
   */
  WavAudioOutput(const std::string &filename, unsigned int samplerate,
                 bool stereo);

  virtual ~WavAudioOutput() override;
  virtual bool write(const SampleVector &samples) override;

private:
  /** (Re-)Write .WAV header. */
  bool write_header(unsigned int nsamples);

  static void encode_chunk_id(std::uint8_t *ptr, const char *chunkname);

  template <typename T> static void set_value(std::uint8_t *ptr, T value);

  const unsigned numberOfChannels;
  const unsigned sampleRate;
  std::FILE *m_stream;
  std::vector<std::uint8_t> m_bytebuf;
};

class PortAudioOutput : public AudioOutput {
public:
  //
  // Construct PortAudio output stream.
  //
  // device_index :: device index number
  // samplerate   :: audio sample rate in Hz
  // stereo       :: true if the output stream contains stereo data
  PortAudioOutput(const PaDeviceIndex device_index, unsigned int samplerate,
                  bool stereo);

  virtual ~PortAudioOutput() override;
  virtual bool write(const SampleVector &samples) override;

private:
  // Terminate PortAudio
  // then add PortAudio error string to m_error and set m_zombie flag.
  void add_paerror(const std::string &msg);

  unsigned int m_nchannels;
  PaStreamParameters m_outputparams;
  PaStream *m_stream;
  PaError m_paerror;
  std::vector<std::uint8_t> m_bytebuf;
};

#endif
