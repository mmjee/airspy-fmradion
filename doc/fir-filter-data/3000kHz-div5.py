#!/usr/bin/env python3
from scipy import signal
import matplotlib.pyplot as plt
import numpy as np

b = [
-148.0111722921044760E-6,
-491.4094674801370390E-6,
-718.1672338107945280E-6,
-16.29626735281832590E-6,
 0.002298277364638320,
 0.005503663598096803,
 0.006642206314553497,
 0.001682126832507245,
-0.010915505153393089,
-0.026184852212141906,
-0.031860772610841082,
-0.013379826345781939,
 0.036756501345089856,
 0.110944368884442118,
 0.186198352921351312,
 0.233742325091577025,
 0.233742325091577025,
 0.186198352921351312,
 0.110944368884442118,
 0.036756501345089856,
-0.013379826345781939,
-0.031860772610841082,
-0.026184852212141906,
-0.010915505153393089,
 0.001682126832507245,
 0.006642206314553497,
 0.005503663598096803,
 0.002298277364638320,
-16.29626735281832590E-6,
-718.1672338107945280E-6,
-491.4094674801370390E-6,
-148.0111722921044760E-6
]

w, h = signal.freqz(b)
fig, ax1 = plt.subplots()
ax1.set_title('Digital filter frequency response')
ax1.plot(w, 20 * np.log10(abs(h)), 'b')
ax1.set_ylabel('Amplitude [dB]', color='b')
ax1.set_xlabel('Frequency [rad/sample]')
plt.show()
