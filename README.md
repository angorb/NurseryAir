# NurseryAir

Don't wake the baby, but know if they're stinky.

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
