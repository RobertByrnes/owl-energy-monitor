# Sending the readings to your own server (optional extra)

An earlier version of the sketch could forward its readings to a web server of your own.
That code has been taken out of the main sketch, because it was switched off by default and
every unused feature is something more to keep secure. This page records exactly what was
removed and how to put it back, so it also works as an example of passing the data on.

The version below fixes the one real weakness of the old code. The old version sent the API key
over plain HTTP. This one **only talks HTTPS and checks the server's certificate**.

## What it did

Every 3 minutes, once the first reading had arrived, the board sent one form-encoded POST:

| Field | Value |
|-------|-------|
| `api_key` | The secret from `config.h`, so your server can tell the request is yours. |
| `watts` | Current demand in watts. |
| `kwHours` | kWh used so far today. |
| `nowCost` | What you are spending right now, in pounds per hour. |
| `dayCost` | Cost so far today, in pounds. |

The same request from a command line, which is also how to test your server:

```
curl -sS https://yourserver.com/api/endpoint -d "api_key=YourAPIKeyValue&watts=1234.00&kwHours=5.68&nowCost=0.28&dayCost=1.23"
```

## What was removed

| File | Removed |
|------|---------|
| `owl_energy_display.ino` | `#define ENABLE_HTTP_POST 0`, the `#include <HTTPClient.h>` that depended on it, `POST_INTERVAL_MS`, the function `postReadings()`, the block at the end of `loop()`, and the `serverName, apiKeyValue` line in the header comment. |
| `config.h` and `config.example.h` | `serverName` and `apiKeyValue`. |
| `README.md` | The `ENABLE_HTTP_POST` row, the `serverName` row, the "Optional" bullet and the plain-HTTP security note. |

## Putting it back

Do these five steps in order. The first line of each block says where it goes.

### 1. Your settings

Add these to `config.h` for real, and to `config.example.h` with placeholders. The certificate
is public information, not a secret, but keep it in `config.h` so the sketch stays tidy.

```cpp
// config.h (and config.example.h)
const char* serverName   = "https://yourserver.com/api/endpoint";   // must start with https://
String      apiKeyValue  = "YourAPIKeyValue";
// The CA certificate that signed your server's certificate, as PEM text (see "Getting the certificate").
const char* serverRootCA = R"PEM(-----BEGIN CERTIFICATE-----
PASTE_THE_CERTIFICATE_HERE
-----END CERTIFICATE-----
)PEM";
```

### 2. Includes

```cpp
// owl_energy_display.ino - with the other #include lines
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
```

### 3. The interval

```cpp
// owl_energy_display.ino - with the other settings
const uint32_t  POST_INTERVAL_MS = 180000UL;   // how often to send the readings (3 minutes)
```

### 4. The function

```cpp
// owl_energy_display.ino - above the OTA section
/**
 * Sends the latest readings to serverName as a form POST over HTTPS. Blocks for a few
 * seconds: the TLS handshake, then up to 5 seconds waiting for the reply.
 */
void postReadings() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (strncmp(serverName, "https://", 8) != 0) return;   // never send the key over plain HTTP
  struct tm now;
  if (!getLocalTime(&now, 0)) return;          // the certificate check needs the real date

  WiFiClientSecure client;
  client.setCACert(serverRootCA);              // only trust a server whose certificate chains to this CA
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, serverName)) return;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = String("api_key=") + apiKeyValue +
                "&watts="   + String(latest.watts, 2) +
                "&kwHours=" + String(latest.kwhToday, 2) +
                "&nowCost=" + String(latest.costNow, 2) +
                "&dayCost=" + String(latest.costToday, 2);
  DBG("POST: watts=%.2f kWh=%.2f\n", latest.watts, latest.kwhToday);   // never log the body: it holds the API key
  int code = http.POST(body);
  DBG("POST response: %d\n", code);
  (void)code;                                  // only used by the debug log
  http.end();
}
```

### 5. Call it from `loop()`

```cpp
// owl_energy_display.ino - in loop(), after "if (haveData) updateInfoBox();"
  static uint32_t lastPostMs = 0;
  if (haveData && millis() - lastPostMs >= POST_INTERVAL_MS) {
    lastPostMs = millis();
    postReadings();
  }
```

The first post goes out about 3 minutes after the board starts. The readings it sends have
already passed the sketch's range checks, so your server only ever gets believable numbers
from the board, but it should check them again (see below).

If you'd like an on/off switch like the old sketch had, wrap the include, the constant, the
function and the `loop()` block in `#if ENABLE_HTTP_POST ... #endif` and add
`#define ENABLE_HTTP_POST 1` to the options at the top.

## Getting the certificate

Use the certificate of the **CA that signed your server's certificate** (the root, or the
intermediate), not the server's own certificate. Your server's own certificate is replaced every
few months when it renews, which would break the sketch each time.

- **In a browser:** open your site, click the padlock, view the certificate, open the *Details* or
  *Certification Path* tab, select the top certificate in the chain, and export it as
  **Base64 / PEM**. Paste the text, including the `BEGIN` and `END` lines, into `serverRootCA`.
- **With OpenSSL:** `openssl s_client -showcerts -connect yourserver.com:443 </dev/null` prints every
  certificate in the chain. Use the last one.

Never use `client.setInsecure()` to get past a certificate error. It turns off the check, and then
anyone on your network can pretend to be your server and collect the API key.

## What your server should do

- Accept HTTPS only, and refuse plain HTTP.
- Compare `api_key` using a constant-time comparison (`hash_equals` in PHP, `hmac.compare_digest` in
  Python), and answer 401 or 403 if it is wrong.
- Use a key that can only add readings, so a leaked key can't read or delete anything else.
- Check that the four values are numbers in a sensible range before storing them, and use
  parameterised queries if they go into a database.
- Don't write the key to your logs.

## Things to know

- **The screen pauses briefly.** The post blocks `loop()` for a couple of seconds (longer if the server
  is slow), once every 3 minutes. The dial freezes meanwhile and an OTA update would wait.
- **It needs the clock.** The sketch starts NTP at boot, and `postReadings()` skips the post until the
  date is known, because a certificate can't be checked without it.
- **It posts the last readings even if the OWL has gone quiet.** To stop that, change the `if` in step 5
  to `if (haveData && millis() - lastPacketMs < STALE_AFTER_MS && millis() - lastPostMs >= POST_INTERVAL_MS)`.
- **CA certificates expire.** If posts start failing years from now, check whether the CA changed and
  refresh `serverRootCA`.
- **Memory.** TLS needs roughly 40 KB of free heap while it runs. That's fine on an ESP32 with this
  sketch.

## Troubleshooting

| Problem | What to do |
|---------|------------|
| Serial shows `POST response: -1` (or another negative number) | The connection or the certificate check failed. Check the server name, that port 443 is reachable from the board, that `serverRootCA` is the right CA, and that the clock is set. |
| `POST response: 401` or `403` | The server rejected the key. Check `apiKeyValue` matches the one the server expects. |
| `POST response: 200` but nothing is stored | The request got through, so look at the server side: the field names above, and the server's own logs. |
| Nothing is posted and Serial shows no `POST:` line | `serverName` doesn't start with `https://`, or the clock isn't set yet. |
| `Multiple libraries were found for "HTTPClient.h"`, or odd errors from it | An old copy of `HTTPClient` in `Documents\Arduino\libraries` is hiding the ESP32 one. Remove or rename that folder. |
