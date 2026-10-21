# OWL Display

A live electricity monitor for an ESP32 and a 480×320 colour TFT. It listens for the
multicast packets sent by an **OWL Intuition ("Network OWL")** energy monitor on your
home network and shows what your house is using right now.

```
┌───────────────────────────┬──────────────────────┐
│        Owl Monitor        │ Usage Today          │
│                           │ 5.68 kWh             │
│       ( heat dial )       ├──────────────────────┤
│            1234           │ Cost Today           │
│           Watts           │ 1.23 GBP             │
│                           ├──────────────────────┤
│                           │ Rate                 │
│                           │ 0.28 GBP/hr          │
│                           ├──────────────────────┤
├───────────────────────────┤ Owl Battery          │
│    IP  <->  last packet   │ 85 %                 │
└───────────────────────────┴──────────────────────┘
```

- **Heat dial:** live demand in watts. It runs from cool blue through green, yellow and
  orange to red hot at full scale (3,500 W by default). The part of the dial not reached
  yet shows as a dim version of the same colours.
- **Usage Today / Cost Today:** kWh and money so far today.
- **Rate:** what you are spending per hour right now.
- **Owl Battery:** battery level of the OWL sensor.
- **Bottom-left box:** switches every 5 seconds between the device's IP address and the
  time the last packet arrived. After 3 minutes of silence it reads `No data since hh:mm:ss`,
  and it shows `WIFI LOST` if the connection drops.
- **Updates over WiFi (OTA):** password protected, with a progress bar on the screen.
- **Safe by default:** only packets sent to the OWL's multicast group are used, and a packet with an
  impossible-looking value is ignored whole (see "How it works").

Only the parts of the screen that change are redrawn, so it doesn't flicker.

Forwarding the readings to your own web server isn't part of the sketch, but
[README_HTTP_POST.md](README_HTTP_POST.md) has the code and steps to add it as an extra.

## What you need

| Item | Notes |
|------|-------|
| ESP32 board with a 480×320 8-bit parallel TFT shield | Developed on a Wemos/TTGO **D1 R32**-style board (Arduino UNO form factor) with a shield supported by [MCUFRIEND_kbv](https://github.com/prenticedavid/MCUFRIEND_kbv). The TFT pins are fixed by the shield and defined in that library, so there is no pin setup in the sketch. |
| OWL Intuition energy monitor | Must be sending its multicast packets on the same network as the ESP32. The sketch listens on `224.192.32.19`, port `22600`. |
| 2.4 GHz WiFi | The ESP32 only does 2.4 GHz. |
| Arduino IDE 2.x | See below. |

Tested with: **arduino-esp32 3.3.12**, **Adafruit GFX Library 1.13.0**, **MCUFRIEND_kbv 3.0.0**.

## Repository layout

```
OWL_Display/
├── README.md
├── README_HTTP_POST.md         optional extra: send the readings to your own server
├── .gitignore                  keeps config.h, backups and keys out of git
├── config.example.h            template for your private settings
└── owl_energy_display/         the Arduino sketch
    ├── owl_energy_display.ino  the program
    ├── colours.h               colour constants (see "Screen colours")
    └── config.h                YOUR settings: you create this, git ignores it
```

## Setting up the Arduino IDE

### 1. Install the ESP32 board support

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. Open **File → Preferences** and add this to *Additional boards manager URLs*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Open **Tools → Board → Boards Manager**, search for **esp32** and install
   **"esp32 by Espressif Systems"** (version 3.3.x).

### 2. Install the libraries

Open **Tools → Manage Libraries** and install:

- **Adafruit GFX Library** (say yes to installing *Adafruit BusIO* when asked)
- **MCUFRIEND_kbv** (by David Prentice)

`WiFi`, `AsyncUDP`, `ArduinoOTA`, `ESPmDNS` and `Update` all come with the ESP32
board support, so don't install anything for those. The font header `FreeDefaultFonts.h`
comes with MCUFRIEND_kbv.

### 3. Patch MCUFRIEND_kbv for ESP32 core 3.x

With version 3.x of the ESP32 core, MCUFRIEND_kbv fails to build with
`'GPIO' was not declared in this scope`. Fix it once:

1. Open `Documents\Arduino\libraries\MCUFRIEND_kbv\utility\mcufriend_shield.h`.
2. Find the ESP32 section, which starts with this line:
   ```cpp
   #elif defined(ESP32)       //regular UNO shield on TTGO D1 R32 (ESP32)
   ```
3. Add this line directly underneath it:
   ```cpp
   #include "soc/gpio_struct.h"
   ```

Updating the library from Library Manager overwrites the file, so reapply the patch if the
error comes back.

### 4. Create your config file

Your WiFi details and passwords live in `config.h`, which is **not** stored in git. Copy the
template into the sketch folder and fill it in:

```
copy config.example.h owl_energy_display\config.h        (Windows)
cp config.example.h owl_energy_display/config.h          (macOS / Linux)
```

| Setting | Meaning |
|---------|---------|
| `ssid`, `password` | Your 2.4 GHz WiFi name and password. |
| `otaPasswordHash` | The **SHA-256 hash** of your OTA password (64 hex characters), not the password itself. See "Making the OTA password hash" below. If it is missing or malformed, OTA stays switched off and Serial says why. |

Older versions of the sketch also read `serverName` and `apiKeyValue` from this file. Nothing uses
them any more, so you can delete them from an old `config.h`.

#### Making the OTA password hash

`config.h` stores only a hash of the password you use to update over WiFi, so the password
itself never sits in a file, in git or in a synced folder. Pick a long, random password
(16 or more characters; a password manager's generator is ideal), then turn it into its hash.
This asks for the password without showing it:

```
python -c "import hashlib,getpass; print(hashlib.sha256(getpass.getpass().encode()).hexdigest())"
```

Paste the 64-character result into `otaPasswordHash` in `config.h`, and keep the real password in
your password manager, because the Arduino IDE asks for it when you upload over WiFi. (On Windows
use `py` instead of `python` if `python` isn't found.) With no Python, PowerShell can do it, but it
shows what you type, so clear the screen afterwards:

```
$pw = Read-Host "OTA password"
$h = [Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($pw))
($h | ForEach-Object { $_.ToString("x2") }) -join ""
```

### 5. Choose the board settings and upload

1. Open `owl_energy_display/owl_energy_display.ino`.
2. **Tools → Board → esp32 →** *ESP32 Dev Module* (or your board's own entry if it has one).
3. **Tools → Partition Scheme →** *Default 4MB with spiffs*. Use a scheme that has OTA
   slots. Don't pick "No OTA" or "Huge APP", or updating over WiFi won't work.
4. **Tools → Port →** the board's COM port.
5. Click **Upload**. If it sits on "Connecting…", hold the board's **BOOT** button until
   it starts.
6. Open **Tools → Serial Monitor** at **115200 baud**. A good start looks like this (the
   `Listening` line needs `DEBUG_SERIAL` on, which is the default):
   ```
   Wifi Connected!
   OTA ready: owl-display.local
   Listening for OWL packets on 224.192.32.19:22600
   ```

On the screen you'll see *Connecting to WiFi…*, then *Wifi Connected!* with the IP address, then
*Waiting for the OWL to Hoot!* until the first packet arrives. The dashboard then appears.

## Updating over WiFi (OTA)

The first upload has to be over USB. After that:

1. Make sure the PC and the ESP32 are on the same network.
2. In **Tools → Port**, look under **Network ports** for **owl-display at 192.168.x.x**.
3. Upload as normal. The IDE asks for the password: type your real OTA password, not the hash.
4. The screen shows a progress bar, then restarts. If an update fails, the error shows for a
   few seconds and the normal screen returns.

If the network port doesn't appear, check that your router isn't blocking device-to-device
traffic (client isolation) and that mDNS is allowed. As a fallback, use USB.

## Settings in the sketch

The options at the top of `owl_energy_display.ino` are the ones you'll most likely want to change.

| Setting | Default | What it does |
|---------|---------|--------------|
| `ENABLE_OTA` | `1` | Allow password-protected updates over WiFi. |
| `DEBUG_SERIAL` | `1` | Print every packet and parsed value to Serial. Set to `0` once it's working. |
| `MAX_WATTS` and the other `MAX_` limits | 25,000 W, 600,000 Wh, £50/hr, £1,000/day | The biggest readings the sketch believes. A packet with anything beyond them is ignored. They are generous on purpose; raise `MAX_WATTS` if your supply really is bigger than about 23 kW. |
| `METER_MAX_WATTS` | `3500` | Full scale of the dial. Lower it to make the dial go red sooner. |
| `METER_GHOST` | `45` | Brightness (0-255) of the unlit part of the dial. |
| `HEAT_STOPS` | blue → red | The colours of the dial and where each one sits. |
| `STALE_AFTER_MS` | 3 minutes | Time without a packet before the box says `No data since…`. |
| `INFO_PAGE_MS` | 5 seconds | How long each page of the bottom-left box shows. |
| `TIMEZONE`, `NTP_SERVER` | UK time, `pool.ntp.org` | Clock used for the "last packet" time. Until the clock is set, the box shows `Last packet: 12s ago` instead. |
| `OTA_HOSTNAME` | `owl-display` | Name the board uses on the network (`owl-display.local`). |

## How it works

1. The ESP32 joins the OWL's multicast group and receives each packet as it is sent.
2. The packet handler throws away anything that wasn't sent to that group, such as packets
   aimed straight at the board. It then copies the rest into a small queue.
3. `loop()` reads each packet and updates the values: watts, kWh, battery, and cost (the sketch
   treats the OWL's cost figures as pence and shows them as pounds).
   A packet that lacks a field leaves the old value alone.
4. Every number is checked first. If any value in a packet isn't a real number, or is beyond
   the `MAX_` limits, the whole packet is ignored: the readings don't change, and it doesn't
   count as having heard from the OWL, so a stream of bad packets can't keep the display
   looking alive.
5. The display redraws only what changed.
6. If WiFi drops, the board rejoins the multicast group automatically when it's back.

### Screen colours

This screen runs **inverted** (`tft.invertDisplay(true)`), so every colour sent to it is the
*complement* of what appears. That's why `colours.h` has `WHITE` as `0x0000` and `BLACK` as
`0xFFFF`. The dial's `heatColour()` function does the same. If you add colours of your own,
send `~colour565`.

## Troubleshooting

| Problem | What to do |
|---------|------------|
| `'GPIO' was not declared in this scope` | Apply the MCUFRIEND_kbv patch in step 3 above. |
| `'otaPasswordHash' was not declared` | Your `config.h` is missing or out of date. Copy it again from `config.example.h` and fill it in. |
| Serial says `OTA disabled: otaPasswordHash ...` | The value must be exactly 64 hex characters, the SHA-256 of your password, and not the hash of an empty password. |
| The IDE says the OTA password is wrong | Type the original password, not the hash. If it still fails, remake the hash and check the password had no stray space or newline. |
| Errors about `ArduinoOTA`, such as `setHostname`, or the build says it is using `Documents\Arduino\libraries\ArduinoOTA` | That folder is a different library, for SAMD and Uno R4 boards, and it is hiding the ESP32 one. Move it aside for ESP32 builds. |
| Stays on "Waiting for the OWL to Hoot!" | No packets are arriving. Check the OWL is sending multicast packets on the same network and subnet as the ESP32. Some routers and access points block or throttle multicast (look for IGMP snooping or multicast settings). Serial should show `Listening for OWL packets`. |
| Bottom-left box says `No data since…` | Nothing has been heard for 3 minutes. Check the OWL and its sensor battery. Serial may say why: see the next two rows. |
| Serial says `Ignored a packet not addressed to the OWL group (from 192.168.x.x)` | A packet reached the board that wasn't sent to `224.192.32.19`. If that address is not your OWL, it is something else on your network and can be ignored. If it is your OWL, then either it is using a different multicast group (change `OWL_GROUP` to match) or it is sending straight to the board, which this sketch deliberately doesn't accept. Serial shows this note at most once every 10 seconds. |
| Serial says `Ignored a packet: watts is not a believable value` (or battery, cost, usage) | That packet was outside the `MAX_` limits or wasn't a number, so it was dropped. If it was your real OWL and your supply is bigger than about 23 kW, raise `MAX_WATTS`. Otherwise it is a corrupt or forged packet and dropping it is correct. |
| `WIFI LOST` | The board will reconnect by itself. Check the router if it doesn't. |
| Restarts over and over at "WiFi failed!" | Check `ssid` and `password`, and that you're on 2.4 GHz. |

## Security notes

- Never commit `config.h`. `.gitignore` covers it, along with any other `config.*` file except
  `config.example.h`, plus `*.bak`, `*.orig`, `*.pem` and `*.key`, so a stray backup or key file
  can't be added by accident. If your sketchbook lives in OneDrive or Dropbox, `config.h` will
  sync there too.
- `config.h` keeps only a **hash** of the OTA password. Treat the hash as secret too: with this
  login scheme, anyone who has the hash could write a tool that logs in. The WiFi password still
  has to be plain text in `config.h` because the board needs it exactly as it is.
- Use a long, random OTA password. Updates are password protected but not encrypted or signed,
  so anyone who has the password (or the hash) and is on your network can load new firmware.
  Don't forward the OTA port to the internet.
- The board only accepts packets sent to the OWL's multicast group, and ignores any packet with a
  value that isn't a believable number. That stops packets aimed straight at the board, and garbage
  values, from reaching the screen.
- The OWL's multicast packets aren't signed or authenticated, so there is nothing more the board can
  check. Another device on your network that joins the group could still send false but believable
  readings. That only affects what the display shows, so keep the OWL and the ESP32 on a network
  you trust.
- If you add the optional server upload from [README_HTTP_POST.md](README_HTTP_POST.md), it uses HTTPS
  and checks the server's certificate. Don't switch the certificate check off.
- Set `DEBUG_SERIAL` to `0` once everything works.

## Credits

Built on the [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library),
[MCUFRIEND_kbv](https://github.com/prenticedavid/MCUFRIEND_kbv) by David Prentice, and the
[arduino-esp32](https://github.com/espressif/arduino-esp32) core.
