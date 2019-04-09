# airspy-fmradion

* Version v0.6.3, 10-APR-2019
* For MacOS and Linux
* *NOTE: this release has a major change adding the AM reception function*

### What is airspy-fmradion?

* **airspy-fmradion** is a software-defined radio receiver for FM broadcast radio, specifically designed for Airspy R2 and Airspy HF+, and RTL-SDR.
* This repository is forked from [ngsoftfm-jj1bdx](https://github.com/jj1bdx/ngsoftfm-jj1bdx) 0.1.14 and merged with [airspfhf-fmradion](https://github.com/jj1bdx/airspyhf-fmradion)

### What does airspy-fmradion provide?

- mono or stereo decoding of FM broadcasting stations
- buffered real-time playback to soundcard or dumping to file
- command-line interface (*only*)

## Usage

```sh
airspy-fmradion -t airspy -q \
    -c freq=88100000,srate=10000000,lgain=2,mgain=0,vgain=10 \
    -b 1.0 -R - | \
    play -t raw -esigned-integer -b16 -r 48000 -c 2 -q -

airspy-fmradion -t airspyhf -q \
    -c freq=88100000 \
    -b 1.0 -R - | \
    play -t raw -esigned-integer -b16 -r 48000 -c 2 -q -

airspy-fmradion -m am -t airspyhf -q \
    -c freq=666000 \
    -b 0.5 -F - | \
    play --buffer=1024 -t raw -e floating-point -b32 -r 48000 -c 1 -q -
```

### airspy-fmradion requires

 - Linux / macOS
 - C++11 (gcc, clang/llvm)
 - [Airspy library](https://github.com/airspy/airspyone_host)
 - [Airspy HF library](https://github.com/airspy/airspyhf)
 - [RTL-SDR library](http://sdr.osmocom.org/trac/wiki/rtl-sdr)
 - [sox](http://sox.sourceforge.net/)
 - [The SoX Resampler library aka libsoxr](https://sourceforge.net/p/soxr/wiki/Home/)
 - Tested: Airspy R2 and Airspy HF+, RTL-SDR V3
 - Fast computer
 - Medium-strong FM radio signal

For the latest version, see https://github.com/jj1bdx/airspy-fmradion

### Branches and tags

  - Official releases are tagged
  - _master_ is the "production" branch with the most stable release (often ahead of the latest release though)
  - _dev_ is the development branch that contains current developments that will be eventually released in the master branch
  - Other branches are experimental (and abandoned)

## Prerequisites

### Required libraries

If you install from source in your own installation path, you have to specify the include path and library path.
For example if you installed it in `/opt/install/libairspy` you have to add `-DAIRSPY_INCLUDE_DIR=/opt/install/libairspy/include -DAIRSPYHF_INCLUDE_DIR=/opt/install/libairspyhf/include` to the cmake options.

### Debian/Ubuntu Linux

Base requirements:

  - `sudo apt-get install cmake pkg-config libusb-1.0-0-dev libasound2-dev libboost-all-dev`

To install the library from a Debian/Ubuntu installation just do:

  - `sudo apt-get install libairspy-dev libairspyhf-dev librtlsdr-dev libsoxr-dev`

### macOS

* Install HomeBrew `airspy`, `airspyhf`, `rtl-sdr`, and `libsoxr`
* See <https://github.com/pothosware/homebrew-pothos/wiki>
* Use HEAD for `airspyhf`

```shell
brew tap pothosware/homebrew-pothos
brew tap dholm/homebrew-sdr #other sdr apps
brew update
brew install libsoxr
brew install rtl-sdr
brew install airspy
brew install airspyhf --HEAD
```

## Installing

To install airspy-fmradion, download and unpack the source code and go to the
top level directory. Then do like this:

 - `mkdir build`
 - `cd build`
 - `cmake ..`

CMake tries to find librtlsdr. If this fails, you need to specify
the location of the library in one the following ways:

```shell
cmake .. \
  -DAIRSPY_INCLUDE_DIR=/path/airspy/include \
  -DAIRSPY_LIBRARY_PATH=/path/airspy/lib/libairspy.a
  -DAIRSPYHF_INCLUDE_DIR=/path/airspyhf/include \
  -DAIRSPYHF_LIBRARY_PATH=/path/airspyhf/lib/libairspyhf.a \
  -DRTLSDR_INCLUDE_DIR=/path/rtlsdr/include \
  -DRTLSDR_LIBRARY_PATH=/path/rtlsdr/lib/librtlsdr.a

PKG_CONFIG_PATH=/path/to/airspy/lib/pkgconfig cmake ..
```

Compile and install

 - `make -j4` (for machines with 4 CPUs)
 - `make install`

## Basic command options

 - `-m devtype` is modulation type, either `fm` or `am` (default fm)
 - `-t devtype` is mandatory and must be `airspy` for Airspy R2 and `airspyhf` for Airspy HF+.
 - `-q` Quiet mode.
 - `-c config` Comma separated list of configuration options as key=value pairs or just key for switches. Depends on device type (see next paragraph).
 - `-d devidx` Device index, 'list' to show device list (default 0)
 - `-M` Disable stereo decoding
 - `-R filename` Write audio data as raw `S16_LE` samples. Use filename `-` to write to stdout
 - `-F filename` Write audio data as raw `FLOAT_LE` samples. Use filename `-` to write to stdout
 - `-W filename` Write audio data to .WAV file
 - `-P [device]` Play audio via ALSA device (default `default`). Use `aplay -L` to get the list of devices for your system
 - `-T filename` Write pulse-per-second timestamps. Use filename '-' to write to stdout
 - `-b seconds` Set audio buffer size in seconds
 - `-X` Shift pilot phase (for Quadrature Multipath Monitor) (-X is ignored under mono mode (-M))
 - `-U` Set deemphasis to 75 microseconds (default: 50)
 - `-f` Set AM Filter type: default: +-6kHz, middle: +-4kHz, narrow: +-3kHz

## Major changes

### Audio gain adjustment

* Since v0.4.2, output maximum level is back at -6dB (0.5) (`adjust_gain()` is reintroduced) again, as in pre-v0.2.7
* During v0.2.7 to v0.4.1, output level was at unity (`adjust_gain()` is removed)
* Before v0.2.7, output maximum level is at -6dB (0.5) 

### Audio downsampling is now performed by libsoxr

* Output of FM demodulator is downsampled by libsoxr to 192kHz
* Output of the stereo decoder is downsampled by libsoxr to 48kHz
* Quality: `SOXR_HQ` (`SOXR_VHQ` is overkill)
* 19kHz cut LPF implemented for post-processing libsoxr output

## No-goals

* Adaptive IF filtering (unable to obtain better results)
* CIC filters for the IF 1st stage (unable to explore parallelism, too complex to compensate)

## Filter design documentation

### General characteristics

* Aliasing allowed outside the -90dB width for the 1st-stage IF filters to reduce CPU power

### For FM

* FM Filter coefficients are listed under `doc/filter-design-fm`
* FM unused filter coefficients are listed under `doc/filter-design-fm/not-used`
* DiscriminatorEqualizer IF range: 200kHz ~ 1MHz (nyquist: 100kHz ~ 500kHz)
* -90dB IF filter width: +-~188kHz for RTL-SDR and AirSpy R2, +-~138kHz for AirSpy HF+
* <0.01dB IF filter width: at least +-75kHz for all receivers

### For AM

* AM Filter coefficients are listed under `doc/filter-design-am`
* AM IF filters are configured by the downsampling rate only
* Up to -1dB rolloff allowed for all IF filters
* Max +-6kHz IF filter width without aliasing set for all IF filters
* Narrower filters by `-f` options: `middle` +-4kHz, `narrow` +-3kHz

## AM AGC

* IF AGC: gain up to 80dB (10000)
* Audio AGC: gain up to 7dB (5.0), fast AGC with peak detection

## Airspy R2 modification from ngsoftfm-jj1bdx

### The modification strategy

[Twitter @lambdaprog suggested the following strategy](https://twitter.com/lambdaprog/status/1101495337292910594):

> Try starting with 10MSPS and that small conversion filter (7 taps vs. the standard 47 taps), then decimate down to ~312.5 ksps (decimation by 32), then feed the FM demod. The overall CPU usage will be very low and the bit growth will give 14.5 bit resolution.

### Removed features

* Halfband kernel filter designed by Twitter @lambdaprog is set for Airspy conversion filter
* Finetuner is removed (Not really needed for +-1ppm or less offset)
* Audio sample rate is fixed to 48000Hz

### Conversion process

* An integer downsampler is added to the first-stage LowPassFilterFirIQ (LPFIQ)
* Use pre-built optimized filter coefficients for LPFIQ and audio filters
* Input -> LPFIQ 1st -> LPFIQ 2nd -> PhaseDiscriminator
* 10MHz -> 312.5kHz (/32)
* 2.5MHz -> 312.5kHz (/8)
* Use `AIRSPY_SAMPLE_FLOAT32_IQ` to directly obtain float IQ sample data from Airspy: IF level is now -24.08dB than the previous (pre-v0.2.2) version
* Use sparse debug output for ppm and other level status
* CPU usage: ~56% -> ~31% on Mac mini 2018, with debug output on, comparing with ngsoftfm-jj1bdx 0.1.14
* More optimization on LPFIQ and audio filters by assuming symmetric coefficients (-5% of Mac mini 2018 CPU usage)

### Airspy configuration options

  - `freq=<int>` Desired tune frequency in Hz. Valid range from 1M to 1.8G. (default 100M: `100000000`)
  - `srate=<int>` Device sample rate. `list` lists valid values and exits. (default `10000000`). Valid values depend on the Airspy firmware. Airspy firmware and library must support dynamic sample rate query.
  - `lgain=<x>` LNA gain in dB. Valid values are: `0, 1, 2, 3, 4, 5, 6, 7, 8 ,9 ,10, 11 12, 13, 14, list`. `list` lists valid values and exits. (default `8`)
  - `mgain=<x>` Mixer gain in dB. Valid values are: `0, 1, 2, 3, 4, 5, 6, 7, 8 ,9 ,10, 11 12, 13, 14, 15, list`. `list` lists valid values and exits. (default `8`)
  - `vgain=<x>` VGA gain in dB. Valid values are: `0, 1, 2, 3, 4, 5, 6, 7, 8 ,9 ,10, 11 12, 13, 14, 15, list`. `list` lists valid values and exits. (default `0`)
  - `antbias` Turn on the antenna bias for remote LNA (default off)
  - `lagc` Turn on the LNA AGC (default off)
  - `magc` Turn on the mixer AGC (default off)

## Airspy HF+ modification from airspy-fmradion v0.2.7

## Conversion process

* LPFIQ is single-stage
* IF center frequency is down Fs/4 than the station frequency, i.e: when the station is 76.5MHz, the tuned frequency is 76.308MHz
* Airspy HF+ allows only 660kHz alias-free BW, so the maximum alias-free BW for IF is (660/2)kHz - 192kHz = 138kHz
* FM demodulation rate: 384kHz (/2)
* 48 * 16 = 768, so all filters are in integer sampling rates

### Filter characteristics

* IF first stage (768kHz -> 384kHz) : <-0.01dB: 80kHz, -3dB: 100kHz, -90dB: 135kHz

### Airspy HF configuration options

  - `freq=<int>` Desired tune frequency in Hz. Valid range from 0 to 31M, and from 60M to 240M. (default 100M: `100000000`)
  - `srate=<int>` Device sample rate. `list` lists valid values and exits. (default `768000`). Valid values depend on the Airspy HF firmware. Airspy HF firmware and library must support dynamic sample rate query.

## RTL-SDR

### Sample rate

* Valid sample rates are from 900001 to 937500 [Hz].
* The default value is 937500Hz.
* FM demodulation rate: 300~312.5kHz (/3)

### Conversion process

* No decimation
* The audio stage is the same as in Airspy

### RTL-SDR configuration options

  - `freq=<int>` Desired tune frequency in Hz. Accepted range from 10M to 2.2G.
(default 100M: `100000000`)
  - `gain=<x>` (default `auto`)
    - `auto` Selects gain automatically
    - `list` Lists available gains and exit
    - `<float>` gain in dB. Possible gains in dB are: `0.0, 0.9, 1.4, 2.7, 3.7,
7.7, 8.7, 12.5, 14.4, 15.7, 16.6, 19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8, 33.8
, 36.4, 37.2, 38.6, 40.2, 42.1, 43.4, 43.9, 44.5, 48.0, 49.6`
  - `srate=<int>` Device sample rate. valid values in the [225001, 300000], [900001, 3200000] ranges. (default `1000000`)
  - `blklen=<int>` Device block length in bytes (default RTL-SDR default i.e. 64k)
  - `agc` Activates device AGC (default off)
  - `antbias` Turn on the antenna bias for remote LNA (default off)

## Authors

* Joris van Rantwijk
* Edouard Griffiths, F4EXB (no longer involving in maintaining NGSoftFM)
* Kenji Rikitake, JJ1BDX (maintainer)
* András Retzler, HA7ILM (for AM AGC code in [csdr](https://github.com/simonyiszk/csdr))

## Acknowledgments

* Twitter [@lambdaprog](https://twitter.com/lambdaprog/)

## License

* As a whole package: GPLv3 (and later). See [LICENSE](LICENSE).
* [csdr](https://github.com/simonyiszk/csdr) AGC code: BSD license.
* Some source code files are stating GPL "v2 and later" license.
