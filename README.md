# NurseryAir

Do you have a stinky baby? Have a better idea with less risk of waking them up!

### Hardware

- [SparkFun ESP32-WROOM Thing Plus C](https://www.sparkfun.com/sparkfun-thing-plus-esp32-wroom-usb-c.html)
- [SparkFun BME680 Environmental Sensor Breakout](https://www.sparkfun.com/sparkfun-environmental-sensor-breakout-bme680-qwiic.html)

The pins and i2c address are configurable in `src/main.cpp`, so it may work with other development boards and BME680 breakouts.

STLs for a 3D-printable hardware-free project box is available in `/models`. The models can be customized and regenerated to fit other hardware by editing and running `models/make_enclosure.py` (requires numpy and matplotlib).

### Files

- `main.cpp` has the web server, saved settings, and history.
- `web_ui.h` holds the single-page UI, embedded in the firmware.
- `secrets.h` gained WEB_USER and WEB_PASS, so set those before flashing.

### What you get

- Dashboard: temperature (tap to switch °C/°F), humidity, gas resistance, and gas as a percent of baseline. A status badge shows Learning, Normal, or Odor detected.
- 12-hour chart: gas resistance, baseline, and the alert threshold line, with alert markers.
- “Mark diaper change” button: a green marker on the chart, so you can see how well alerts line up with real events. This is the best tool for tuning.
- Settings: threshold, re-arm level, samples before alert, cooldown, warm-up time, ntfy on/off, and the ntfy URL. Changes are validated and survive reboots.
- Actions: send a test alert, or reset the baseline (useful after moving the sensor or changing the room setup).

### Using it

- Open http://nursery-air.local (or the IP shown in the serial monitor). Some Android browsers don’t resolve .local, so use the IP there.
- It uses basic auth over plain HTTP, which is fine on a home network but shouldn’t be exposed to the internet.
- History is kept in RAM (one point per minute), so it resets on reboot. TODO: Saving it to flash would be a reasonable next step.
- The ntfy URL must be http://, because the code doesn’t do TLS. Also, anyone who can read the topic name can read your alerts, so keep it long and random.
