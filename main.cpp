// airspy-fmradion - Software decoder for FM broadcast radio with RTL-SDR
//
// Copyright (C) 2013, Joris van Rantwijk.
// Copyright (C) 2015 Edouard Griffiths, F4EXB
// Copyright (C) 2018, 2019, 2020 Kenji Rikitake, JJ1BDX
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, see http://www.gnu.org/licenses/gpl-2.0.html

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include "AirspyHFSource.h"
#include "AirspySource.h"
#include "AmDecode.h"
#include "AudioOutput.h"
#include "DataBuffer.h"
#include "FileSource.h"
#include "FilterParameters.h"
#include "FmDecode.h"
#include "FourthConverterIQ.h"
#include "MovingAverage.h"
#include "NbfmDecode.h"
#include "RtlSdrSource.h"
#include "SoftFM.h"
#include "Utility.h"

// define this for enabling coefficient monitor functions
// #undef COEFF_MONITOR

#define AIRSPY_FMRADION_VERSION "20201025-0"

/** Flag is set on SIGINT / SIGTERM. */
static std::atomic_bool stop_flag(false);

/**
 * Get data from output buffer and write to output stream.
 *
 * This code runs in a separate thread.
 */
void write_output_data(AudioOutput *output, DataBuffer<Sample> *buf,
                       unsigned int buf_minfill) {
  while (!stop_flag.load()) {

    if (buf->queued_samples() == 0) {
      // The buffer is empty. Perhaps the output stream is consuming
      // samples faster than we can produce them. Wait until the buffer
      // is back at its nominal level to make sure this does not happen
      // too often.
      buf->wait_buffer_fill(buf_minfill);
    }

    if (buf->pull_end_reached()) {
      // Reached end of stream.
      break;
    }

    // Get samples from buffer and write to output.
    SampleVector samples = buf->pull();
    output->write(samples);
    if (!(*output)) {
      fprintf(stderr, "ERROR: AudioOutput: %s\n", output->error().c_str());
      // Setting stop_flag to true, suggested by GitHub @montgomeryb
      stop_flag.store(true);
    }
  }
}

/** Handle Ctrl-C and SIGTERM. */
static void handle_sigterm(int sig) {
  stop_flag.store(true);

  std::string msg = "\nGot signal ";
  msg += strsignal(sig);
  msg += ", stopping ...\n";

  const char *s = msg.c_str();
  ssize_t size = write(STDERR_FILENO, s, strlen(s));
  size++; // dummy
}

void usage() {
  fprintf(
      stderr,
      "Usage: airspy-fmradion [options]\n"
      "  -m modtype     Modulation type:\n"
      "                   - fm (default)\n"
      "                   - am\n"
      "                   - dsb\n"
      "                   - usb\n"
      "                   - lsb\n"
      "                   - cw (pitch: 500Hz USB)\n"
      "                   - nbfm\n"
      "  -t devtype     Device type:\n"
      "                   - rtlsdr: RTL-SDR devices\n"
      "                   - airspy: Airspy R2\n"
      "                   - airspyhf: Airspy HF+\n"
      "                   - filesource: File Source\n"
      "  -q             Quiet mode\n"
      "  -c config      Comma separated key=value configuration pairs or just "
      "key for switches\n"
      "                 See below for valid values per device type\n"
      "  -d devidx      Device index, 'list' to show device list (default 0)\n"
      "  -M             Disable stereo decoding\n"
      "  -R filename    Write audio data as raw S16_LE samples\n"
      "                 use filename '-' to write to stdout\n"
      "  -F filename    Write audio data as raw FLOAT_LE samples\n"
      "                 use filename '-' to write to stdout\n"
      "  -W filename    Write audio data to .WAV file\n"
      "  -P device_num  Play audio via PortAudio device index number\n"
      "                 use string '-' to specify the default PortAudio "
      "device\n"
      "  -T filename    Write pulse-per-second timestamps\n"
      "                 use filename '-' to write to stdout\n"
      "  -b seconds     Set audio buffer size in seconds (default: 1 second)\n"
      "  -X             Shift pilot phase (for Quadrature Multipath Monitor)\n"
      "                 (-X is ignored under mono mode (-M))\n"
      "  -U             Set deemphasis to 75 microseconds (default: 50)\n"
      "  -f filtername  Filter type:\n"
      "                 For FM:\n"
      "                   - wide: same as default\n"
      "                   - default: none after conversion\n"
      "                   - medium:  +-156kHz\n"
      "                   - narrow:  +-121kHz\n"
      "                 For AM:\n"
      "                   - wide: +-9kHz\n"
      "                   - default: +-6kHz\n"
      "                   - medium:  +-4.5kHz\n"
      "                   - narrow:  +-3kHz\n"
      "                 For NBFM:\n"
      "                   - wide: +-20kHz, with +-17kHz deviation\n"
      "                   - default: +-10kHz\n"
      "                   - medium:  +-8kHz\n"
      "                   - narrow:  +-6.25kHz\n"
      "  -l dB          Set IF squelch level to minus given value of dB\n"
      "  -E stages      Enable multipath filter for FM\n"
      "                 (For stable reception only:\n"
      "                  turn off if reception becomes unstable)\n"
      "  -r ppm         Set IF offset in ppm (range: +-1000000ppm)\n"
      "                 (This option affects output pitch and timing:\n"
      "                  use for the output timing compensation only!)\n"
      "\n"
      "Configuration options for RTL-SDR devices\n"
      "  freq=<int>     Frequency of radio station in Hz (default 100000000)\n"
      "                 valid values: 10M to 2.2G (working range depends on "
      "device)\n"
      "  srate=<int>    IF sample rate in Hz (default 937500)\n"
      "                 (valid ranges: [225001, 300000], [900001, 3200000]))\n"
      "  gain=<float>   Set LNA gain in dB, or 'auto',\n"
      "                 or 'list' to just get a list of valid values (default "
      "auto)\n"
      "  blklen=<int>   Set audio buffer size in seconds (default RTL-SDR "
      "default)\n"
      "  agc            Enable RTL AGC mode (default disabled)\n"
      "  antbias        Enable antenna bias (default disabled)\n"
      "\n"
      "Configuration options for Airspy devices:\n"
      "  freq=<int>     Frequency of radio station in Hz (default 100000000)\n"
      "                 valid values: 24M to 1.8G\n"
      "  srate=<int>    IF sample rate in Hz. Depends on Airspy firmware and "
      "libairspy support\n"
      "                 Airspy firmware and library must support dynamic "
      "sample rate query. (default 10000000)\n"
      "  lgain=<int>    LNA gain in dB. 'list' to just get a list of valid "
      "values: (default 8)\n"
      "  mgain=<int>    Mixer gain in dB. 'list' to just get a list of valid "
      "values: (default 8)\n"
      "  vgain=<int>    VGA gain in dB. 'list' to just get a list of valid "
      "values: (default 8)\n"
      "  antbias        Enable antenna bias (default disabled)\n"
      "  lagc           Enable LNA AGC (default disabled)\n"
      "  magc           Enable mixer AGC (default disabled)\n"
      "\n"
      "Configuration options for Airspy HF devices:\n"
      "  freq=<int>     Frequency of radio station in Hz (default 100000000)\n"
      "                 valid values: 192k to 31M, and 60M to 260M\n"
      "  srate=<int>    IF sample rate in Hz.\n"
      "                 Depends on Airspy HF firmware and libairspyhf support\n"
      "                 Airspy HF firmware and library must support dynamic\n"
      "                 sample rate query. (default 384000)\n"
      "  hf_att=<int>   HF attenuation level and AGC control\n"
      "                 0: enable AGC, no attenuation\n"
      "                 1 ~ 8: disable AGC, apply attenuation of value * 6dB\n"
      "\n"
      "Configuration options for (experimental) FileSource devices:\n"
      "  freq=<int>        Frequency of radio station in Hz\n"
      "  srate=<int>       IF sample rate in Hz.\n"
      "  filename=<string> Source file name.\n"
      "                    Supported encodings: FLOAT, S24_LE, S16_LE\n"
      "  zero_offset       Set if the source file is in zero offset,\n"
      "                    which requires Fs/4 IF shifting.\n"
      "  blklen=<int>      Set block length in samples.\n"
      "  raw               Set if the file is raw binary.\n"
      "  format=<string>   Set the file format for the raw binary file.\n"
      "                    (formats: U8_LE, S8_LE, S16_LE, S24_LE, FLOAT)\n"
      "\n");
}

void badarg(const char *label) {
  usage();
  fprintf(stderr, "ERROR: Invalid argument for %s\n", label);
  exit(1);
}

bool parse_int(const char *s, int &v, bool allow_unit = false) {
  char *endp;
  long t = strtol(s, &endp, 10);
  if (endp == s) {
    return false;
  }
  if (allow_unit && *endp == 'k' && t > INT_MIN / 1000 && t < INT_MAX / 1000) {
    t *= 1000;
    endp++;
  }
  if (*endp != '\0' || t < INT_MIN || t > INT_MAX) {
    return false;
  }
  v = t;
  return true;
}

/** Return Unix time stamp in seconds. */
double get_time() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + 1.0e-6 * tv.tv_usec;
}

static bool get_device(std::vector<std::string> &devnames, DevType devtype,
                       Source **srcsdr, int devidx) {
  // Get device names.
  switch (devtype) {
  case DevType::RTLSDR:
    RtlSdrSource::get_device_names(devnames);
    break;
  case DevType::Airspy:
    AirspySource::get_device_names(devnames);
    break;
  case DevType::AirspyHF:
    AirspyHFSource::get_device_names(devnames);
    break;
  case DevType::FileSource:
    FileSource::get_device_names(devnames);
    break;
  }

  if (devidx < 0 || (unsigned int)devidx >= devnames.size()) {
    if (devidx != -1) {
      fprintf(stderr, "ERROR: invalid device index %d\n", devidx);
    }

    fprintf(stderr, "Found %u devices:\n", (unsigned int)devnames.size());

    for (unsigned int i = 0; i < devnames.size(); i++) {
      fprintf(stderr, "%2u: %s\n", i, devnames[i].c_str());
    }

    return false;
  }

  fprintf(stderr, "using device %d: %s\n", devidx, devnames[devidx].c_str());

  // Open receiver devices.
  switch (devtype) {
  case DevType::RTLSDR:
    *srcsdr = new RtlSdrSource(devidx);
    break;
  case DevType::Airspy:
    *srcsdr = new AirspySource(devidx);
    break;
  case DevType::AirspyHF:
    *srcsdr = new AirspyHFSource(devidx);
    break;
  case DevType::FileSource:
    *srcsdr = new FileSource(devidx);
    break;
  }

  return true;
}

int main(int argc, char **argv) {

  int devidx = 0;
  int pcmrate = FmDecoder::sample_rate_pcm;
  bool stereo = true;
  OutputMode outmode = OutputMode::RAW_INT16;
  std::string filename("-");
  int portaudiodev = -1;
  bool quietmode = false;
  std::string ppsfilename;
  FILE *ppsfile = nullptr;
  double bufsecs = -1;
  bool enable_squelch = false;
  double squelch_level_db = 150.0;
  bool pilot_shift = false;
  bool deemphasis_na = false;
  int multipathfilter_stages = 0;
  bool ifrate_offset_enable = false;
  double ifrate_offset_ppm = 0;
  std::string config_str;
  std::string devtype_str;
  DevType devtype;
  std::string modtype_str("fm");
  ModType modtype = ModType::FM;
  std::string filtertype_str("default");
  FilterType filtertype = FilterType::Default;
  std::vector<std::string> devnames;
  Source *srcsdr = 0;

  fprintf(stderr, "airspy-fmradion " AIRSPY_FMRADION_VERSION "\n");
  fprintf(stderr, "Software FM/AM radio for ");
  fprintf(stderr, "Airspy R2, Airspy HF+, and RTL-SDR\n");

  const struct option longopts[] = {
      {"modtype", optional_argument, nullptr, 'm'},
      {"devtype", optional_argument, nullptr, 't'},
      {"quiet", required_argument, nullptr, 'q'},
      {"config", optional_argument, nullptr, 'c'},
      {"dev", required_argument, nullptr, 'd'},
      {"mono", no_argument, nullptr, 'M'},
      {"raw", required_argument, nullptr, 'R'},
      {"float", required_argument, nullptr, 'F'},
      {"wav", required_argument, nullptr, 'W'},
      {"play", optional_argument, nullptr, 'P'},
      {"pps", required_argument, nullptr, 'T'},
      {"buffer", required_argument, nullptr, 'b'},
      {"pilotshift", no_argument, nullptr, 'X'},
      {"usa", no_argument, nullptr, 'U'},
      {"filtertype", optional_argument, nullptr, 'f'},
      {"squelch", required_argument, nullptr, 'l'},
      {"multipathfilter", required_argument, nullptr, 'E'},
      {"ifrateppm", optional_argument, nullptr, 'r'},
      {nullptr, no_argument, nullptr, 0}};

  int c, longindex;
  while ((c = getopt_long(argc, argv, "m:t:c:d:MR:F:W:f:l:P:T:b:qXUE:r:",
                          longopts, &longindex)) >= 0) {
    switch (c) {
    case 'm':
      modtype_str.assign(optarg);
      break;
    case 't':
      devtype_str.assign(optarg);
      break;
    case 'c':
      config_str.assign(optarg);
      break;
    case 'd':
      if (!parse_int(optarg, devidx)) {
        devidx = -1;
      }
      break;
    case 'M':
      stereo = false;
      break;
    case 'R':
      outmode = OutputMode::RAW_INT16;
      filename = optarg;
      break;
    case 'F':
      outmode = OutputMode::RAW_FLOAT32;
      filename = optarg;
      break;
    case 'W':
      outmode = OutputMode::WAV;
      filename = optarg;
      break;
    case 'f':
      filtertype_str.assign(optarg);
      break;
    case 'l':
      if (!Utility::parse_dbl(optarg, squelch_level_db) ||
          squelch_level_db < 0) {
        badarg("-l");
      }
      enable_squelch = true;
      break;
    case 'P':
      outmode = OutputMode::PORTAUDIO;
      if (0 == strncmp(optarg, "-", 1)) {
        portaudiodev = -1;
      } else if (!parse_int(optarg, portaudiodev) || portaudiodev < 0) {
        badarg("-P");
      }
      break;
    case 'T':
      ppsfilename = optarg;
      break;
    case 'b':
      if (!Utility::parse_dbl(optarg, bufsecs) || bufsecs < 0) {
        badarg("-b");
      }
      break;
    case 'q':
      quietmode = true;
      break;
    case 'X':
      pilot_shift = true;
      break;
    case 'U':
      deemphasis_na = true;
      break;
    case 'E':
      if (!parse_int(optarg, multipathfilter_stages) ||
          multipathfilter_stages < 1) {
        badarg("-E");
      }
      break;
    case 'r':
      ifrate_offset_enable = true;
      if (!Utility::parse_dbl(optarg, ifrate_offset_ppm) ||
          std::fabs(ifrate_offset_ppm) > 1000000.0) {
        badarg("-r");
      }
      break;
    default:
      usage();
      fprintf(stderr, "ERROR: Invalid command line options\n");
      exit(1);
    }
  }

  if (optind < argc) {
    usage();
    fprintf(stderr, "ERROR: Unexpected command line options\n");
    exit(1);
  }

  double squelch_level;
  if (enable_squelch) {
    squelch_level = pow(10.0, -(squelch_level_db / 20.0));
  } else {
    squelch_level = 0;
  }

  if (strcasecmp(devtype_str.c_str(), "rtlsdr") == 0) {
    devtype = DevType::RTLSDR;
  } else if (strcasecmp(devtype_str.c_str(), "airspy") == 0) {
    devtype = DevType::Airspy;
  } else if (strcasecmp(devtype_str.c_str(), "airspyhf") == 0) {
    devtype = DevType::AirspyHF;
  } else if (strcasecmp(devtype_str.c_str(), "filesource") == 0) {
    devtype = DevType::FileSource;
  } else {
    fprintf(
        stderr,
        "ERROR: wrong device type (-t option) must be one of the following:\n");
    fprintf(stderr, "        rtlsdr, airspy, airspyhf, filesource\n");
    exit(1);
  }

  if (strcasecmp(modtype_str.c_str(), "fm") == 0) {
    modtype = ModType::FM;
  } else if (strcasecmp(modtype_str.c_str(), "am") == 0) {
    modtype = ModType::AM;
    stereo = false;
  } else if (strcasecmp(modtype_str.c_str(), "dsb") == 0) {
    modtype = ModType::DSB;
    stereo = false;
  } else if (strcasecmp(modtype_str.c_str(), "usb") == 0) {
    modtype = ModType::USB;
    stereo = false;
  } else if (strcasecmp(modtype_str.c_str(), "lsb") == 0) {
    modtype = ModType::LSB;
    stereo = false;
  } else if (strcasecmp(modtype_str.c_str(), "cw") == 0) {
    modtype = ModType::CW;
    stereo = false;
  } else if (strcasecmp(modtype_str.c_str(), "nbfm") == 0) {
    modtype = ModType::NBFM;
    stereo = false;
  } else {
    fprintf(stderr, "Modulation type string unsuppored\n");
    exit(1);
  }

  if (strcasecmp(filtertype_str.c_str(), "default") == 0) {
    filtertype = FilterType::Default;
  } else if (strcasecmp(filtertype_str.c_str(), "medium") == 0) {
    filtertype = FilterType::Medium;
  } else if (strcasecmp(filtertype_str.c_str(), "narrow") == 0) {
    filtertype = FilterType::Narrow;
  } else if (strcasecmp(filtertype_str.c_str(), "wide") == 0) {
    filtertype = FilterType::Wide;
  } else {
    fprintf(stderr, "Filter type string unsuppored\n");
    exit(1);
  }

  // Catch Ctrl-C and SIGTERM
  struct sigaction sigact;
  sigact.sa_handler = handle_sigterm;
  sigemptyset(&sigact.sa_mask);
  sigact.sa_flags = SA_RESETHAND;

  if (sigaction(SIGINT, &sigact, nullptr) < 0) {
    fprintf(stderr, "WARNING: can not install SIGINT handler (%s)\n",
            strerror(errno));
  }

  if (sigaction(SIGTERM, &sigact, nullptr) < 0) {
    fprintf(stderr, "WARNING: can not install SIGTERM handler (%s)\n",
            strerror(errno));
  }

  // Open PPS file.
  if (!ppsfilename.empty()) {
    if (ppsfilename == "-") {
      fprintf(stderr, "writing pulse-per-second markers to stdout\n");
      ppsfile = stdout;
    } else {
      fprintf(stderr, "writing pulse-per-second markers to '%s'\n",
              ppsfilename.c_str());
      ppsfile = fopen(ppsfilename.c_str(), "w");

      if (ppsfile == nullptr) {
        fprintf(stderr, "ERROR: can not open '%s' (%s)\n", ppsfilename.c_str(),
                strerror(errno));
        exit(1);
      }
    }

    switch (modtype) {
    case ModType::FM:
      fprintf(ppsfile, "#pps_index sample_index   unix_time\n");
      break;
    case ModType::AM:
    case ModType::DSB:
    case ModType::USB:
    case ModType::LSB:
    case ModType::CW:
    case ModType::NBFM:
      fprintf(ppsfile, "#  block   unix_time\n");
      break;
    }
    fflush(ppsfile);
  }

  // Calculate number of samples in audio buffer.
  // Set default buffer length to 1 second.
  unsigned int outputbuf_samples = pcmrate;
  // if bufsecs is explicitly set in the command line (not negative),
  // set the calculate number of samples for configured buffer length.
  if (bufsecs > 0) {
    // Calculate numberr of samples for configured buffer length.
    outputbuf_samples = (unsigned int)(bufsecs * pcmrate);
  }
  // Set minimum limit for the output buffer length.
  if (outputbuf_samples < 480) {
    outputbuf_samples = 480;
  }
  fprintf(stderr, "output buffer length: %g [s]\n",
          outputbuf_samples / double(pcmrate));

  // Prepare output writer.
  std::unique_ptr<AudioOutput> audio_output;

  switch (outmode) {
  case OutputMode::RAW_INT16:
    fprintf(stderr,
            "writing raw 16-bit integer little-endian audio samples to '%s'\n",
            filename.c_str());
    audio_output.reset(new RawAudioOutput(filename));
    audio_output->SetConvertFunction(AudioOutput::samplesToInt16);
    break;
  case OutputMode::RAW_FLOAT32:
    fprintf(stderr,
            "writing raw 32-bit float little-endian audio samples to '%s'\n",
            filename.c_str());
    audio_output.reset(new RawAudioOutput(filename));
    audio_output->SetConvertFunction(AudioOutput::samplesToFloat32);
    break;
  case OutputMode::WAV:
    fprintf(stderr, "writing audio samples to '%s'\n", filename.c_str());
    audio_output.reset(new WavAudioOutput(filename, pcmrate, stereo));
    break;
  case OutputMode::PORTAUDIO:
    if (portaudiodev == -1) {
      fprintf(stderr, "playing audio to PortAudio default device: ");
    } else {
      fprintf(stderr, "playing audio to PortAudio device %d: ", portaudiodev);
    }
    audio_output.reset(new PortAudioOutput(portaudiodev, pcmrate, stereo));
    fprintf(stderr, "name '%s'\n", audio_output->get_device_name().c_str());
    break;
  }

  if (!(*audio_output)) {
    fprintf(stderr, "ERROR: AudioOutput: %s\n", audio_output->error().c_str());
    exit(1);
  }

  if (!get_device(devnames, devtype, &srcsdr, devidx)) {
    exit(1);
  }

  if (!(*srcsdr)) {
    fprintf(stderr, "ERROR source: %s\n", srcsdr->error().c_str());
    delete srcsdr;
    exit(1);
  }

  // Configure device and start streaming.
  if (!srcsdr->configure(config_str)) {
    fprintf(stderr, "ERROR: configuration: %s\n", srcsdr->error().c_str());
    delete srcsdr;
    exit(1);
  }

  double freq = srcsdr->get_configured_frequency();
  fprintf(stderr, "tuned for %.7g [MHz]", freq * 1.0e-6);
  double tuner_freq = srcsdr->get_frequency();
  if (tuner_freq != freq) {
    fprintf(stderr, ", device tuned for %.7g [MHz]", tuner_freq * 1.0e-6);
  }
  fprintf(stderr, "\n");

  double ifrate = srcsdr->get_sample_rate();

  unsigned int if_blocksize;

  bool enable_fs_fourth_downconverter = !(srcsdr->is_low_if());

  bool enable_downsampling = true;
  double if_decimation_ratio = 1.0;
  double fm_target_rate = FmDecoder::sample_rate_if;
  double am_target_rate = AmDecoder::internal_rate_pcm;
  double nbfm_target_rate = NbfmDecoder::internal_rate_pcm;

  // Configure blocksize.
  switch (devtype) {
  case DevType::Airspy:
    if_blocksize = 65536;
    break;
  case DevType::AirspyHF:
    // Common settings.
    if_blocksize = 2048;
    break;
  case DevType::RTLSDR:
    if_blocksize = 65536;
    break;
  case DevType::FileSource:
    if_blocksize = 2048;
    break;
  }

  // IF rate compensation if requested.
  if (ifrate_offset_enable) {
    ifrate *= 1.0 + (ifrate_offset_ppm / 1000000.0);
  }

  // Configure if_decimation_ratio.
  switch (modtype) {
  case ModType::FM:
    if_decimation_ratio = ifrate / fm_target_rate;
    break;
  case ModType::AM:
  case ModType::DSB:
  case ModType::USB:
  case ModType::LSB:
  case ModType::CW:
    if_decimation_ratio = ifrate / am_target_rate;
    break;
  case ModType::NBFM:
    if_decimation_ratio = ifrate / nbfm_target_rate;
    break;
  }

  // Show decoding modulation type.
  fprintf(stderr, "Decoding modulation type: %s\n", modtype_str.c_str());
  if (enable_squelch) {
    fprintf(stderr, "IF Squelch level: %.9g [dB]\n", 20 * log10(squelch_level));
  }

  double demodulator_rate = ifrate / if_decimation_ratio;
  double total_decimation_ratio = ifrate / pcmrate;
  double audio_decimation_ratio = demodulator_rate / pcmrate;

  // Display ifrate compensation if applicable.
  if (ifrate_offset_enable) {
    fprintf(stderr, "IF sample rate shifted by: %.9g [ppm]\n",
            ifrate_offset_ppm);
  }

  // Display filter configuration.
  fprintf(stderr, "IF sample rate: %.9g [Hz], ", ifrate);
  fprintf(stderr, "IF decimation: / %.9g\n", if_decimation_ratio);
  fprintf(stderr, "Demodulator rate: %.8g [Hz], ", demodulator_rate);
  fprintf(stderr, "audio decimation: / %.9g\n", audio_decimation_ratio);

  srcsdr->print_specific_parms();

  // Create source data queue.
  DataBuffer<IQSample> source_buffer;

  // ownership will be transferred to thread therefore the unique_ptr with
  // move is convenient if the pointer is to be shared with the main thread
  // use shared_ptr (and no move) instead
  std::unique_ptr<Source> up_srcsdr(srcsdr);

  // Start reading from device in separate thread.
  up_srcsdr->start(&source_buffer, &stop_flag);

  // Reported by GitHub @bstalk: (!up_srcadr) doesn't work for gcc of Debian.
  if (!(*up_srcsdr)) {
    fprintf(stderr, "ERROR: source: %s\n", up_srcsdr->error().c_str());
    exit(1);
  }

  // Prevent aliasing at very low output sample rates.
  double deemphasis = deemphasis_na ? FmDecoder::default_deemphasis_na
                                    : FmDecoder::default_deemphasis_eu;

  // Prepare Fs/4 downconverter.
  FourthConverterIQ fourth_downconverter(false);

  IfResampler if_resampler(ifrate,          // input_rate
                           demodulator_rate // output_rate
  );
  enable_downsampling = (ifrate != demodulator_rate);

  IQSampleCoeff amfilter_coeff;
  IQSampleCoeff fmfilter_coeff;
  IQSampleCoeff nbfmfilter_coeff;

  switch (filtertype) {
  case FilterType::Default:
    amfilter_coeff = FilterParameters::jj1bdx_am_48khz_default;
    fmfilter_coeff = FilterParameters::delay_3taps_only_iq;
    nbfmfilter_coeff = FilterParameters::jj1bdx_nbfm_48khz_default;
    break;
  case FilterType::Medium:
    amfilter_coeff = FilterParameters::jj1bdx_am_48khz_medium;
    fmfilter_coeff = FilterParameters::jj1bdx_fm_384kHz_medium;
    nbfmfilter_coeff = FilterParameters::jj1bdx_nbfm_48khz_medium;
    break;
  case FilterType::Narrow:
    amfilter_coeff = FilterParameters::jj1bdx_am_48khz_narrow;
    fmfilter_coeff = FilterParameters::jj1bdx_fm_384kHz_narrow;
    nbfmfilter_coeff = FilterParameters::jj1bdx_nbfm_48khz_narrow;
    break;
  case FilterType::Wide:
    amfilter_coeff = FilterParameters::jj1bdx_am_48khz_wide;
    fmfilter_coeff = FilterParameters::delay_3taps_only_iq;
    nbfmfilter_coeff = FilterParameters::jj1bdx_nbfm_48khz_wide;
    break;
  }

  // Prepare AM decoder.
  AmDecoder am(amfilter_coeff, // amfilter_coeff
               modtype         // mode
  );

  // Prepare FM decoder.
  FmDecoder fm(fmfilter_coeff, // fmfilter_coeff
               stereo,         // stereo
               deemphasis,     // deemphasis,
               pilot_shift,    // pilot_shift
               static_cast<unsigned int>(multipathfilter_stages)
               // multipath_stages
  );

  // Prepare narrow band FM decoder.
  NbfmDecoder nbfm(nbfmfilter_coeff,            // nbfmfilter_coeff
                   NbfmDecoder::freq_dev_normal // freq_dev
  );

  // Initialize moving average object for FM ppm monitoring.
  switch (modtype) {
  case ModType::FM:
  case ModType::NBFM:
    fprintf(stderr, "audio sample rate: %u [Hz],", pcmrate);
    fprintf(stderr, " audio bandwidth: %u [Hz]\n",
            (unsigned int)FmDecoder::bandwidth_pcm);
    fprintf(stderr, "audio totally decimated from IF by: %.9g\n",
            total_decimation_ratio);
    break;
  case ModType::AM:
  case ModType::DSB:
  case ModType::USB:
  case ModType::LSB:
  case ModType::CW:
    fprintf(stderr, "AM demodulator deemphasis: %.9g [µs]\n",
            AmDecoder::default_deemphasis);
    break;
  }
  if (modtype == ModType::FM) {
    fprintf(stderr, "FM demodulator deemphasis: %.9g [µs]\n", deemphasis);
    if (multipathfilter_stages > 0) {
      fprintf(stderr, "FM IF multipath filter enabled, stages: %d\n",
              multipathfilter_stages);
    }
  }
  fprintf(stderr, "Filter type: %s\n", filtertype_str.c_str());

  // Initialize moving average object for FM ppm monitoring.
  MovingAverage<float> ppm_average(100, 0.0f);

  unsigned int nchannel = stereo ? 2 : 1;

  // If buffering enabled, start background output thread.
  DataBuffer<Sample> output_buffer;
  std::thread output_thread;
  // Always use output_thread for smooth output.
  output_thread = std::thread(write_output_data, audio_output.get(),
                              &output_buffer, outputbuf_samples * nchannel);
  SampleVector audiosamples;
  bool inbuf_length_warning = false;
  float audio_level = 0;
  bool got_stereo = false;

  double block_time = get_time();

  // TODO: ~0.1sec / display (should be tuned)
  unsigned int stat_rate =
      lrint(5120 / (if_blocksize / total_decimation_ratio));
  unsigned int discarding_blocks;
  switch (modtype) {
  case ModType::FM:
  case ModType::NBFM:
    discarding_blocks = stat_rate * 4;
    break;
  case ModType::AM:
  case ModType::DSB:
  case ModType::USB:
  case ModType::LSB:
  case ModType::CW:
    discarding_blocks = stat_rate * 2;
    break;
  }

  float if_level = 0;

  // Main loop.
  for (unsigned int block = 0; !stop_flag.load(); block++) {

    // Check for overflow of source buffer.
    if (!inbuf_length_warning && source_buffer.queued_samples() > 10 * ifrate) {
      fprintf(stderr, "\nWARNING: Input buffer is growing (system too slow)\n");
      inbuf_length_warning = true;
    }

    // Pull next block from source buffer.
    IQSampleVector iqsamples = source_buffer.pull();

    IQSampleVector if_shifted_samples;
    IQSampleVector if_samples;

    if (iqsamples.empty()) {
      break;
    }

    double prev_block_time = block_time;
    block_time = get_time();

    // Fine tuning is not needed
    // so long as the stability of the receiver device is
    // within the range of +- 1ppm (~100Hz or less).
    if (enable_fs_fourth_downconverter) {
      // Fs/4 downconvering is required
      // to avoid frequency zero offset
      // because Airspy HF+ and RTL-SDR are Zero IF receivers
      fourth_downconverter.process(iqsamples, if_shifted_samples);
    } else {
      if_shifted_samples = std::move(iqsamples);
    }

    // Downsample IF for the decoder.
    if (enable_downsampling) {
      if_resampler.process(if_shifted_samples, if_samples);
    } else {
      if_samples = std::move(if_shifted_samples);
    }

    // Downsample IF for the decoder.
    bool if_exists = if_samples.size() > 0;
    double if_rms = 0.0;

    if (if_exists) {
      // Decode signal.
      switch (modtype) {
      case ModType::FM:
        // Decode FM signal.
        fm.process(if_samples, audiosamples);
        if_rms = fm.get_if_rms();
        break;
      case ModType::AM:
      case ModType::DSB:
      case ModType::USB:
      case ModType::LSB:
      case ModType::CW:
        // Decode AM/DSB/USB/LSB/CW signals.
        am.process(if_samples, audiosamples);
        if_rms = am.get_if_rms();
        break;
      case ModType::NBFM:
        // Decode narrow band FM signal.
        nbfm.process(if_samples, audiosamples);
        if_rms = nbfm.get_if_rms();
        break;
      }
      // Measure the average IF level.
      if_level = 0.75 * if_level + 0.25 * if_rms;
    }

    size_t audiosamples_size = audiosamples.size();
    bool audio_exists = audiosamples_size > 0;

    // Measure audio level when audio exists
    if (audio_exists) {
      float audio_mean, audio_rms;
      IQSampleDecodedVector audiosamples_float;
      audiosamples_float.resize(audiosamples_size);
      volk_64f_convert_32f(audiosamples_float.data(), audiosamples.data(),
                           audiosamples_size);
      Utility::samples_mean_rms(audiosamples_float, audio_mean, audio_rms);
      audio_level = 0.95 * audio_level + 0.05 * audio_rms;

      // Set nominal audio volume (-6dB) when IF squelch is open,
      // set to zero volume if the squelch is closed.
      Utility::adjust_gain(audiosamples, if_rms >= squelch_level ? 0.5 : 0.0);
    }

    if (modtype == ModType::FM) {
      // the minus factor is to show the ppm correction
      // to make and not the one made
      ppm_average.feed((fm.get_tuning_offset() / tuner_freq) * -1.0e6);
    } else if (modtype == ModType::NBFM) {
      ppm_average.feed((nbfm.get_tuning_offset() / tuner_freq) * -1.0e6);
    }

    float if_level_db = 20 * log10(if_level);
    float audio_level_db = 20 * log10(audio_level) + 3.01;
    std::size_t buflen = output_buffer.queued_samples();
    double buflen_sec = buflen / nchannel / double(pcmrate);

    // Show status messages for each block if not in quiet mode.
    bool stereo_change = false;
    if (!quietmode) {
      // Stereo detection display
      if (modtype == ModType::FM) {
        stereo_change = (fm.stereo_detected() != got_stereo);
        // Show stereo status.
        if (stereo_change) {
          got_stereo = fm.stereo_detected();
          if (got_stereo) {
            fprintf(stderr, "\ngot stereo signal, pilot level = %.7f\n",
                    fm.get_pilot_level());
          } else {
            fprintf(stderr, "\nlost stereo signal\n");
          }
        }
      }
      switch (modtype) {
      case ModType::FM:
      case ModType::NBFM:
        // Show per-block statistics.
        if (stereo_change ||
            (((block % stat_rate) == 0) && (block > discarding_blocks))) {
          fprintf(stderr,
#ifdef COEFF_MONITOR
                  "blk=%8d:ppm=%+6.2f:IF=%+6.1fdB:AF=%+6.1fdB:buf=%.2fs\n",
#else
                  "\rblk=%8d:ppm=%+6.2f:IF=%+6.1fdB:AF=%+6.1fdB:buf=%.2fs",
#endif
                  block, ppm_average.average(), if_level_db, audio_level_db,
                  buflen_sec);
          fflush(stderr);
        }
        break;
      case ModType::AM:
      case ModType::DSB:
      case ModType::USB:
      case ModType::LSB:
      case ModType::CW:
        // Show per-block statistics without ppm offset.
        double if_agc_gain_db = 20 * log10(am.get_if_agc_current_gain());
        if (((block % stat_rate) == 0) && (block > discarding_blocks)) {
          fprintf(stderr,
                  "\rblk=%8d:IF=%+6.1fdB:AGC=%+6.1fdB:AF=%+6.1fdB:buf=%.2fs",
                  block, if_level_db, if_agc_gain_db, audio_level_db,
                  buflen_sec);
          fflush(stderr);
        }
        break;
      }

#ifdef COEFF_MONITOR
      if ((modtype == ModType::FM) && (multipathfilter_stages > 0) &&
          ((block % (stat_rate * 10)) == 0) && (block > discarding_blocks)) {
        double mf_error = fm.get_multipath_error();
        const MfCoeffVector &mf_coeff = fm.get_multipath_coefficients();
        fprintf(stderr, "block,%u,mf_error,%.9f,mf_coeff,", block, mf_error);
        for (unsigned int i = 0; i < mf_coeff.size(); i++) {
          MfCoeff val = mf_coeff[i];
          fprintf(stderr, "%d,%.9f,%.9f,", i, val.real(), val.imag());
        }
        fprintf(stderr, "\n");
        fflush(stderr);
      }
#endif
    }

    // Write PPS markers.
    if (ppsfile != nullptr) {
      switch (modtype) {
      case ModType::FM:
        for (const PilotPhaseLock::PpsEvent &ev : fm.get_pps_events()) {
          double ts = prev_block_time;
          ts += ev.block_position * (block_time - prev_block_time);
          fprintf(ppsfile, "%8s %14s %18.6f\n",
                  std::to_string(ev.pps_index).c_str(),
                  std::to_string(ev.sample_index).c_str(), ts);
          fflush(ppsfile);
          // Erase the marked event.
          fm.erase_first_pps_event();
        }
        break;
      case ModType::AM:
      case ModType::DSB:
      case ModType::USB:
      case ModType::LSB:
      case ModType::CW:
      case ModType::NBFM:
        if ((block % (stat_rate * 10)) == 0) {
          fprintf(ppsfile, "%8d %18.6f\n", block, prev_block_time);
          fflush(ppsfile);
        }
        break;
      }
    }

    // Throw away first blocks before stereo pilot locking is completed.
    // They are noisy because IF filters are still starting up.
    // (Increased from one to support high sampling rates)
    if ((block > discarding_blocks) && audio_exists) {
      // Write samples to output.
      // Always use buffered write.
      output_buffer.push(std::move(audiosamples));
    }
  }

  fprintf(stderr, "\n");

  // Join background threads.
  up_srcsdr->stop();

  output_buffer.push_end();
  output_thread.join();

  // No cleanup needed; everything handled by destructors

  return 0;
}

/* end */
