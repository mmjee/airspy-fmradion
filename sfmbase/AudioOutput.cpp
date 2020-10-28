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

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "AudioOutput.h"
#include "SoftFM.h"

/* ****************  class AudioOutput  **************** */

// Set type conversion function of samples.
void AudioOutput::SetConvertFunction(
    void (*converter)(const SampleVector &, std::vector<std::uint8_t> &)) {
  m_converter = converter;
}

// Encode a list of samples as signed 16-bit little-endian integers.
void AudioOutput::samplesToInt16(const SampleVector &samples,
                                 std::vector<uint8_t> &bytes) {
  bytes.resize(2 * samples.size());

  SampleVector::const_iterator i = samples.begin();
  SampleVector::const_iterator n = samples.end();
  std::vector<uint8_t>::iterator k = bytes.begin();

  while (i != n) {
    Sample s = *(i++);
    // Limit output within [-1.0, 1.0].
    s = std::max(Sample(-1.0), std::min(Sample(1.0), s));
    // Convert output to [-32767, 32767].
    long v = lrint(s * 32767);
    unsigned long u = v;
    *(k++) = u & 0xff;
    *(k++) = (u >> 8) & 0xff;
  }
}

// Encode a list of samples as signed 32-bit little-endian floats.
// Note: no output range limitation.
void AudioOutput::samplesToFloat32(const SampleVector &samples,
                                   std::vector<uint8_t> &bytes) {
  bytes.resize(4 * samples.size());

  SampleVector::const_iterator i = samples.begin();
  SampleVector::const_iterator n = samples.end();
  std::vector<uint8_t>::iterator k = bytes.begin();

  while (i != n) {
    // Union for converting float and uint32_t.
    union {
      float f;
      uint32_t u32;
    } v;
    Sample s = *(i++);
    // Note: no output range limitation.
    v.f = (float)s;
    uint32_t u = v.u32;
    *(k++) = u & 0xff;
    *(k++) = (u >> 8) & 0xff;
    *(k++) = (u >> 16) & 0xff;
    *(k++) = (u >> 24) & 0xff;
  }
}

/* ****************  class RawAudioOutput  **************** */

// Construct raw audio writer.
RawAudioOutput::RawAudioOutput(const std::string &filename) {
  if (filename == "-") {

    m_fd = STDOUT_FILENO;

  } else {

    m_fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (m_fd < 0) {
      m_error = "can not open '" + filename + "' (" + strerror(errno) + ")";
      m_zombie = true;
      return;
    }
  }

  m_device_name = "RawAudioOutput";
}

// Destructor.
RawAudioOutput::~RawAudioOutput() {
  // Close file descriptor.
  if (m_fd >= 0 && m_fd != STDOUT_FILENO) {
    close(m_fd);
  }
}

// Write audio data.
bool RawAudioOutput::write(const SampleVector &samples) {
  if (m_fd < 0) {
    return false;
  }

  // Convert samples to bytes.
  m_converter(samples, m_bytebuf);

  // Write data.
  std::size_t p = 0;
  std::size_t n = m_bytebuf.size();
  while (p < n) {

    ssize_t k = ::write(m_fd, m_bytebuf.data() + p, n - p);
    if (k <= 0) {
      if (k == 0 || errno != EINTR) {
        m_error = "write failed (";
        m_error += strerror(errno);
        m_error += ")";
        return false;
      }
    } else {
      p += k;
    }
  }

  return true;
}

/* ****************  class WavAudioOutput  **************** */

// Construct .WAV writer.
WavAudioOutput::WavAudioOutput(const std::string &filename,
                               unsigned int samplerate, bool stereo)
    : numberOfChannels(stereo ? 2 : 1), sampleRate(samplerate) {
  m_stream = fopen(filename.c_str(), "wb");
  if (m_stream == nullptr) {
    m_error = "can not open '" + filename + "' (" + strerror(errno) + ")";
    m_zombie = true;
    return;
  }

  // Write initial header with a dummy sample count.
  // This will be replaced with the actual header once the WavFile is closed.
  if (!write_header(0x7fff0000)) {
    m_error = "can not write to '" + filename + "' (" + strerror(errno) + ")";
    m_zombie = true;
  }
  m_device_name = "WavAudioOutput";
}

// Destructor.
WavAudioOutput::~WavAudioOutput() {
  // We need to go back and fill in the header ...

  if (!m_zombie) {

    const unsigned bytesPerSample = 2;

    const long currentPosition = ftell(m_stream);

    assert((currentPosition - 44) % bytesPerSample == 0);

    const unsigned totalNumberOfSamples =
        (currentPosition - 44) / bytesPerSample;

    assert(totalNumberOfSamples % numberOfChannels == 0);

    // Put header in front

    if (fseek(m_stream, 0, SEEK_SET) == 0) {
      write_header(totalNumberOfSamples);
    }
  }

  // Done writing the file

  if (m_stream) {
    fclose(m_stream);
  }
}

// Write audio data.
bool WavAudioOutput::write(const SampleVector &samples) {
  if (m_zombie) {
    return false;
  }

  // Convert samples to bytes.
  samplesToInt16(samples, m_bytebuf);

  // Write samples to file.
  std::size_t k = fwrite(m_bytebuf.data(), 1, m_bytebuf.size(), m_stream);
  if (k != m_bytebuf.size()) {
    m_error = "write failed (";
    m_error += strerror(errno);
    m_error += ")";
    return false;
  }

  return true;
}

// (Re)write .WAV header.
bool WavAudioOutput::write_header(unsigned int nsamples) {
  const unsigned bytesPerSample = 2;
  const unsigned bitsPerSample = 16;

  enum class wFormatTagId {
    WAVE_FORMAT_PCM = 0x0001,
    WAVE_FORMAT_IEEE_FLOAT = 0x0003
  };

  assert(nsamples % numberOfChannels == 0);

  // synthesize header

  uint8_t wavHeader[44];

  encode_chunk_id(wavHeader + 0, "RIFF");
  set_value<uint32_t>(wavHeader + 4, 36 + nsamples * bytesPerSample);
  encode_chunk_id(wavHeader + 8, "WAVE");
  encode_chunk_id(wavHeader + 12, "fmt ");
  set_value<uint32_t>(wavHeader + 16, 16);
  set_value<uint16_t>(wavHeader + 20, static_cast<unsigned short>(
                                          wFormatTagId::WAVE_FORMAT_PCM));
  set_value<uint16_t>(wavHeader + 22, numberOfChannels);
  set_value<uint32_t>(wavHeader + 24, sampleRate); // sample rate
  set_value<uint32_t>(wavHeader + 28, sampleRate * numberOfChannels *
                                          bytesPerSample); // byte rate
  set_value<uint16_t>(wavHeader + 32,
                      numberOfChannels * bytesPerSample); // block size
  set_value<uint16_t>(wavHeader + 34, bitsPerSample);
  encode_chunk_id(wavHeader + 36, "data");
  set_value<uint32_t>(wavHeader + 40, nsamples * bytesPerSample);

  return fwrite(wavHeader, 1, 44, m_stream) == 44;
}

void WavAudioOutput::encode_chunk_id(uint8_t *ptr, const char *chunkname) {
  for (unsigned i = 0; i < 4; ++i) {
    assert(chunkname[i] != '\0');
    ptr[i] = chunkname[i];
  }
  assert(chunkname[4] == '\0');
}

template <typename T> void WavAudioOutput::set_value(uint8_t *ptr, T value) {
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    ptr[i] = value & 0xff;
    value >>= 8;
  }
}

// Class PortAudioOutput

// Construct PortAudio output stream.
PortAudioOutput::PortAudioOutput(const PaDeviceIndex device_index,
                                 unsigned int samplerate, bool stereo) {
  m_nchannels = stereo ? 2 : 1;

  m_paerror = Pa_Initialize();
  if (m_paerror != paNoError) {
    add_paerror("Pa_Initialize()");
    return;
  }

  if (device_index == -1) {
    m_outputparams.device = Pa_GetDefaultOutputDevice();
  } else {
    PaDeviceIndex index = static_cast<PaDeviceIndex>(device_index);
    if (index >= Pa_GetDeviceCount()) {
      add_paerror("Device number out of range");
      return;
    }
    m_outputparams.device = index;
  }
  if (m_outputparams.device == paNoDevice) {
    add_paerror("No default output device");
    return;
  }
  m_device_name = Pa_GetDeviceInfo(m_outputparams.device)->name;

  m_outputparams.channelCount = m_nchannels;
  m_outputparams.sampleFormat = paFloat32;
  m_outputparams.suggestedLatency =
      Pa_GetDeviceInfo(m_outputparams.device)->defaultHighOutputLatency;
  m_outputparams.hostApiSpecificStreamInfo = NULL;

  m_paerror =
      Pa_OpenStream(&m_stream,
                    NULL, // no input
                    &m_outputparams, samplerate, paFramesPerBufferUnspecified,
                    paClipOff, // no clipping
                    NULL,      // no callback, blocking API
                    NULL       // no callback userData
      );
  if (m_paerror != paNoError) {
    add_paerror("Pa_OpenStream()");
    return;
  }

  m_paerror = Pa_StartStream(m_stream);
  if (m_paerror != paNoError) {
    add_paerror("Pa_StartStream()");
    return;
  }
}

// Destructor.
PortAudioOutput::~PortAudioOutput() {
  m_paerror = Pa_StopStream(m_stream);
  if (m_paerror != paNoError) {
    add_paerror("Pa_StopStream()");
    return;
  }
  Pa_Terminate();
}

// Write audio data.
bool PortAudioOutput::write(const SampleVector &samples) {
  if (m_zombie) {
    return false;
  }

  unsigned long sample_size = samples.size();
  // Convert samples to bytes.
  samplesToFloat32(samples, m_bytebuf);

  m_paerror =
      Pa_WriteStream(m_stream, m_bytebuf.data(), sample_size / m_nchannels);
  if (m_paerror == paNoError) {
    return true;
  } else if (m_paerror == paOutputUnderflowed) {
    // This error is benign
    // fprintf(stderr, "paOutputUnderflowed\n");
    return true;
  } else
    add_paerror("Pa_WriteStream()");
  return false;
}

// Terminate PortAudio
// then add PortAudio error string to m_error and set m_zombie flag.
void PortAudioOutput::add_paerror(const std::string &premsg) {
  Pa_Terminate();
  m_error += premsg;
  m_error += ": PortAudio error: (number: ";
  m_error += std::to_string(m_paerror);
  m_error += " message: ";
  m_error += Pa_GetErrorText(m_paerror);
  m_error += ")";
  m_zombie = true;
}

/* end */
