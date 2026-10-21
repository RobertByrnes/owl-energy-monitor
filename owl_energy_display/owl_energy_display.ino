// =============================================================================
//  OWL Intuition energy display  (ESP32 + MCUFRIEND_kbv 480x320 TFT)
//
//  Listens for the OWL hub's multicast UDP packets and shows:
//    - live demand on a ring meter
//    - kWh and cost so far today, current cost per hour, OWL battery level
//    - bottom-left box: alternates between the IP address and the time of the
//      last packet received
//
//  Updates over WiFi (OTA), protected by a password.
//
//  Needs config.h to provide:
//    const char* ssid, password;
//    const char* otaPasswordHash; // SHA-256 of your OTA password, 64 hex characters (if ENABLE_OTA)
//
//  Only packets sent to the OWL multicast group are used, and a packet holding an
//  impossible-looking value is thrown away whole (see "settings" below).
// =============================================================================

#include <WiFi.h>
#include <AsyncUDP.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <Fonts/FreeSans9pt7b.h>
#include "FreeDefaultFonts.h"
#include "colours.h"
#include "config.h"

// ------------------------------------------------------------------ options --
#define ENABLE_OTA   1       // 1 = allow password-protected firmware updates over WiFi
#define DEBUG_SERIAL 1       // 1 = print packets and parsed values to Serial (set 0 when deployed)

#if ENABLE_OTA
  #include <ArduinoOTA.h>
#endif

#if DEBUG_SERIAL
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...) ((void)0)
#endif

// ----------------------------------------------------------------- settings --
const IPAddress OWL_GROUP(224, 192, 32, 19);   // OWL multicast group
const uint16_t  OWL_PORT         = 22600;
const int       METER_MAX_WATTS  = 3500;
const int       METER_GHOST      = 45;                 // brightness (0-255) of the part of the dial not reached yet
const uint32_t  STALE_AFTER_MS   = 180000UL;   // "No data since..." after this long without a packet
const uint32_t  INFO_PAGE_MS     = 5000UL;     // how long each page of the bottom-left box shows
const uint32_t  LISTEN_RETRY_MS  = 5000UL;     // retry joining the multicast group

// What counts as a believable reading. These are deliberately generous: they only exist so a
// corrupt or forged packet can't put garbage on the screen. A packet with any value outside
// them is thrown away whole (raise MAX_WATTS if you have an unusually big supply).
const float MAX_WATTS      = 25000.0f;         // a 100 A single-phase supply is about 23 kW
const float MAX_DAY_WH     = 600000.0f;        // MAX_WATTS for 24 hours
const float MAX_COST_NOW_P = 5000.0f;          // pence per hour (GBP 50/hr)
const float MAX_COST_DAY_P = 100000.0f;        // pence (GBP 1000)

// OTA updates
const char*  OTA_HOSTNAME         = "owl-display";   // shows up as owl-display.local / in the IDE port list
const int    OTA_BAR_X = 60, OTA_BAR_Y = 230, OTA_BAR_W = 360, OTA_BAR_H = 20;   // progress bar

// Clock (for the "last packet" time). UK time with daylight saving; change to suit.
const char* TIMEZONE   = "GMT0BST,M3.5.0/1,M10.5.0";
const char* NTP_SERVER = "pool.ntp.org";

// Screen layout (480 x 320, rotation 1)
const int DIVIDER_X = 260;                     // vertical divider, right-hand panel starts here
const int VALUE_X   = 280;                     // left edge of the values in the right-hand panel
const int INFO_Y    = 280;                     // top of the bottom-left info box
const int RING_X = 6, RING_Y = 30, RING_R = 122;
const int RING_CX = RING_X + RING_R;           // centre of the ring meter
const int RING_CY = RING_Y + RING_R;
const int METER_UNDRAWN = -1000;               // "ring not drawn yet" marker

// ------------------------------------------------------------------- types ---
const size_t PKT_MAX = 768;                    // longest packet we keep (longer ones are truncated)

struct RxPacket {
  char text[PKT_MAX];
};

struct Readings {
  float watts;       // current demand, W
  float kwhToday;    // kWh used so far today
  float costNow;     // pounds per hour right now
  float costToday;   // pounds so far today
  int   battery;     // OWL sensor battery, %
};

struct ValueField {
  int16_t y;         // text baseline
  char    shown[24]; // text currently on screen (so we only redraw on change)
};

enum class ParseResult {
  NotElectricity,    // no electricity fields in the packet (e.g. a weather packet)
  Accepted,          // fields found, all believable: `latest` was updated
  Rejected           // a field held an impossible value: the whole packet was ignored
};

// ----------------------------------------------------------------- globals ---
MCUFRIEND_kbv tft;
AsyncUDP      udp;
QueueHandle_t rxQueue = nullptr;               // UDP task -> loop()

Readings latest = {0, 0, 0, 0, 0};
bool     haveData        = false;              // at least one electricity packet parsed
uint32_t lastPacketMs    = 0;                  // millis() when the last packet arrived
time_t   lastPacketEpoch = 0;                  // wall-clock time of it (0 = clock wasn't set yet)
bool     layoutDrawn     = false;

ValueField fKwh     = {55,  ""};
ValueField fCostDay = {135, ""};
ValueField fCostNow = {215, ""};
ValueField fBattery = {295, ""};
char       shownInfo[32] = "";                 // text currently in the bottom-left box
int        shownWatts = -1;
int        meterAngle = METER_UNDRAWN;         // angle the ring is currently drawn to
bool       otaEnabled = false;                 // true once OTA has been started
unsigned   otaLastPercent = 101;               // last progress value drawn (101 = none yet)

// ------------------------------------------------------------ text helpers ---

/**
 * Prints text in white on black.
 *
 * @param x     left edge of the text
 * @param y     text baseline
 * @param text  string to print
 * @param size  font scale (1 = normal, 2 = double)
 */
void printAt(int x, int y, const char* text, uint8_t size) {
  tft.setTextSize(size);
  tft.setTextColor(WHITE, BLACK);
  tft.setCursor(x, y);
  tft.print(text);
}

/**
 * Prints text horizontally centred inside a column of the screen.
 *
 * @param left   x of the left edge of the column
 * @param width  width of the column
 * @param y      text baseline
 * @param text   string to print
 */
void printCentredIn(int left, int width, int y, const char* text) {
  int16_t  x1, y1;
  uint16_t w, h;
  tft.setTextSize(1);
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  printAt(left + (width - (int)w) / 2 - x1, y, text, 1);
}

/**
 * Prints text horizontally centred on the whole screen.
 *
 * @param y     text baseline
 * @param text  string to print
 */
void printCentred(int y, const char* text) {
  printCentredIn(0, tft.width(), y, text);
}

/**
 * Clears the screen and shows up to three centred lines (startup messages).
 *
 * @param line1  first line, or nullptr to skip
 * @param line2  second line, or nullptr to skip
 * @param line3  third line, or nullptr to skip
 */
void showSplash(const char* line1, const char* line2, const char* line3) {
  tft.fillScreen(BLACK);
  if (line1) printCentred(120, line1);
  if (line2) printCentred(160, line2);
  if (line3) printCentred(200, line3);
}

/**
 * Shows the "connected, waiting for the first packet" screen with the current IP.
 */
void showWaitingScreen() {
  String ipLine = "IP Address: " + WiFi.localIP().toString();
  showSplash("Wifi Connected!", ipLine.c_str(), "Waiting for the OWL to Hoot!");
}

// ------------------------------------------------------------------ colours ---
// The screen runs inverted: what we send is the complement of what appears (that is why
// BLACK is 0xFFFF and WHITE is 0x0000 in Colours.h). The dial colours do the same.

// Colour stops of the dial, from cool to red hot. pos is the position along the dial
// (0-255); r, g, b are the colours you actually see there.
struct HeatStop {
  uint8_t pos, r, g, b;
};

const HeatStop HEAT_STOPS[] = {
  {  0,   0,  80, 255},   // cool blue (low use)
  { 45,   0, 215, 255},   // cyan
  { 90,   0, 230,  60},   // green
  {135, 240, 225,   0},   // yellow
  {175, 255, 130,   0},   // orange
  {210, 255,  25,   0},   // red
  {255, 255,   0,   0}    // red hot (full scale)
};
const int HEAT_STOP_COUNT = sizeof(HEAT_STOPS) / sizeof(HEAT_STOPS[0]);

/**
 * Colour for a position on the dial, blended smoothly between the HEAT_STOPS.
 *
 * @param pos         0 (cool blue) to 255 (red hot)
 * @param brightness  0-255; 255 is the full colour, lower values give a dimmed version
 * @return            16 bit colour, already complemented for this screen
 */
uint16_t heatColour(int pos, int brightness) {
  pos = constrain(pos, 0, 255);
  int k = 1;
  while (k < HEAT_STOP_COUNT - 1 && pos > HEAT_STOPS[k].pos) k++;
  const HeatStop& lo = HEAT_STOPS[k - 1];
  const HeatStop& hi = HEAT_STOPS[k];
  const int span = hi.pos - lo.pos;
  const int t    = pos - lo.pos;

  const int r = ((lo.r * (span - t) + hi.r * t) / span) * brightness / 255;
  const int g = ((lo.g * (span - t) + hi.g * t) / span) * brightness / 255;
  const int b = ((lo.b * (span - t) + hi.b * t) / span) * brightness / 255;

  const uint16_t colour = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  return (uint16_t)~colour;
}

// -------------------------------------------------------------- ring meter ---

/**
 * Draws the ring meter for a wattage: cool blue at the start of the dial heating up
 * through green, yellow and orange to red hot at full scale. The part not reached yet
 * shows as a dim version of the same colours. Only the segments that changed since the
 * last call are redrawn, so there is no flicker.
 *
 * @param watts  current demand in watts (clamped to 0..METER_MAX_WATTS)
 */
void drawRingMeter(int watts) {
  const int w     = RING_R / 3;   // ring thickness
  const int sweep = 150;          // half of the 300 degree sweep
  const int inc   = 6;            // degrees between segments
  const int seg   = 3;            // width of each segment in degrees
  const int v     = map(constrain(watts, 0, METER_MAX_WATTS), 0, METER_MAX_WATTS, -sweep, sweep);
  const bool fullRedraw = (meterAngle == METER_UNDRAWN);

  for (int i = -sweep + inc / 2; i < sweep - inc / 2; i += inc) {
    const bool lit = i < v;
    if (!fullRedraw && lit == (i < meterAngle)) continue;   // unchanged

    // Segment start (inner and outer edge)
    const float sx = cos((i - 90) * DEG_TO_RAD);
    const float sy = sin((i - 90) * DEG_TO_RAD);
    const int x0 = sx * (RING_R - w) + RING_CX;
    const int y0 = sy * (RING_R - w) + RING_CY;
    const int x1 = sx * RING_R + RING_CX;
    const int y1 = sy * RING_R + RING_CY;

    // Segment end
    const float sx2 = cos((i + seg - 90) * DEG_TO_RAD);
    const float sy2 = sin((i + seg - 90) * DEG_TO_RAD);
    const int x2 = sx2 * (RING_R - w) + RING_CX;
    const int y2 = sy2 * (RING_R - w) + RING_CY;
    const int x3 = sx2 * RING_R + RING_CX;
    const int y3 = sy2 * RING_R + RING_CY;

    const uint16_t colour = heatColour(map(i, -sweep, sweep, 0, 255), lit ? 255 : METER_GHOST);
    tft.fillTriangle(x0, y0, x1, y1, x2, y2, colour);
    tft.fillTriangle(x1, y1, x2, y2, x3, y3, colour);
  }
  meterAngle = v;
}

/**
 * Draws the wattage number in the middle of the ring, replacing the old one.
 *
 * @param watts  current demand in watts
 */
void drawWattsReadout(int watts) {
  char txt[12];
  snprintf(txt, sizeof txt, "%d", watts);

  tft.fillRect(RING_CX - 50, RING_CY - 30, 100, 36, BLACK);   // wipe the old number

  int16_t  x1, y1;
  uint16_t w, h;
  tft.setTextSize(2);
  tft.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  printAt(RING_CX - (int)w / 2 - x1, RING_CY, txt, 2);        // centred in the ring
}

// ----------------------------------------------------------- screen layout ---

/**
 * Clears the screen and draws the fixed parts: labels and divider lines. Also resets
 * the "what's on screen" state so every value is drawn afresh.
 */
void drawLayout() {
  tft.fillScreen(BLACK);

  printCentredIn(RING_X, 2 * RING_R, 20, "Owl Monitor");   // centred over the ring
  printAt(270, 20,  "Usage Today", 1);
  printAt(270, 104, "Cost Today",     1);
  printAt(270, 184, "Rate",           1);
  printAt(270, 264, "Owl Battery",    1);
  printAt(RING_CX - 30, RING_CY + 90, "  Watts", 1);

  const int W = tft.width();
  tft.fillRect(DIVIDER_X, 160, W - DIVIDER_X, 4, OLIVE);
  tft.fillRect(DIVIDER_X, 240, W - DIVIDER_X, 4, OLIVE);
  tft.fillRect(DIVIDER_X, 0, 4, tft.height(), OLIVE);
  tft.fillRect(DIVIDER_X, 80, W - DIVIDER_X, 4, OLIVE);
  tft.fillRect(0, 275, DIVIDER_X, 4, OLIVE);

  // Nothing of the dynamic content is on screen any more
  shownWatts = -1;
  meterAngle = METER_UNDRAWN;
  fKwh.shown[0] = fCostDay.shown[0] = fBattery.shown[0] = fCostNow.shown[0] = '\0';
  shownInfo[0] = '\0';
  layoutDrawn = true;
}

/**
 * Redraws one value in the right-hand panel, but only if its text changed.
 *
 * @param f     the field (position and last drawn text)
 * @param text  new text; an empty string just clears the field
 */
void updateField(ValueField& f, const char* text) {
  if (strcmp(f.shown, text) == 0) return;
  const int clearX = DIVIDER_X + 6;
  tft.fillRect(clearX, f.y - 18, tft.width() - clearX, 26, BLACK);
  if (text[0]) printAt(VALUE_X, f.y, text, 1);
  snprintf(f.shown, sizeof f.shown, "%s", text);
}

/**
 * Shows "<value> <unit>" in a field with two decimals, or nothing while the value is zero.
 *
 * @param f      the field to update
 * @param value  amount to show
 * @param unit   text after the number, e.g. "kWh"
 */
void updateAmount(ValueField& f, float value, const char* unit) {
  char buf[24] = "";
  if (value > 0) snprintf(buf, sizeof buf, "%.2f %s", value, unit);   // hidden while zero
  updateField(f, buf);
}

/**
 * Brings the whole display up to date with `latest`. Draws the layout the first time,
 * then only touches what changed.
 */
void updateDisplay() {
  if (!layoutDrawn) drawLayout();

  const int watts = (int)latest.watts;
  if (watts != shownWatts) {
    drawRingMeter(watts);
    drawWattsReadout(watts);
    shownWatts = watts;
  }

  updateAmount(fKwh,     latest.kwhToday,  "kWh");
  updateAmount(fCostDay, latest.costToday, "GBP");
  updateAmount(fCostNow, latest.costNow,   "GBP/hr");

  char buf[24] = "";
  if (latest.battery > 0) snprintf(buf, sizeof buf, "%d %%", latest.battery);
  updateField(fBattery, buf);
}

/**
 * Turns a time span into short text such as "12s ago", "5m ago" or "3h ago".
 *
 * @param out  buffer for the text
 * @param cap  size of the buffer
 * @param ms   time span in milliseconds
 */
void formatElapsed(char* out, size_t cap, uint32_t ms) {
  const uint32_t s = ms / 1000;
  if (s < 60)        snprintf(out, cap, "%us ago", (unsigned)s);
  else if (s < 3600) snprintf(out, cap, "%um ago", (unsigned)(s / 60));
  else               snprintf(out, cap, "%uh ago", (unsigned)(s / 3600));
}

/**
 * Updates the bottom-left box. It alternates every INFO_PAGE_MS between:
 *   page 1: the IP address (or "WIFI LOST")
 *   page 2: when the last packet arrived. This reads "No data since ..." once nothing
 *           has been heard for STALE_AFTER_MS. If the clock isn't set yet it shows how
 *           long ago instead.
 * Only redraws when the text changes.
 */
void updateInfoBox() {
  if (!layoutDrawn) return;

  char text[32];
  if ((millis() / INFO_PAGE_MS) % 2 == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      const IPAddress ip = WiFi.localIP();
      snprintf(text, sizeof text, "IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    } else {
      snprintf(text, sizeof text, "WIFI LOST");
    }
  } else {
    const bool stale = (millis() - lastPacketMs) > STALE_AFTER_MS;
    if (lastPacketEpoch) {
      struct tm t;
      localtime_r(&lastPacketEpoch, &t);
      snprintf(text, sizeof text, "%s%02d:%02d:%02d",
               stale ? "No data since " : "Last packet: ", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
      char ago[16];
      formatElapsed(ago, sizeof ago, millis() - lastPacketMs);
      snprintf(text, sizeof text, "%s%s", stale ? "No data - " : "Last packet: ", ago);
    }
  }

  if (strcmp(text, shownInfo) == 0) return;
  tft.fillRect(0, INFO_Y, DIVIDER_X, tft.height() - INFO_Y, BLACK);
  printCentredIn(0, DIVIDER_X, 305, text);
  snprintf(shownInfo, sizeof shownInfo, "%s", text);
}

// ----------------------------------------------------------------- parsing ---

/**
 * Copies the text between two tags out of a packet.
 *
 * @param xml       the packet text
 * @param startTag  text just before the wanted part
 * @param endTag    text just after the wanted part
 * @param out       buffer for the result (always terminated)
 * @param cap       size of the buffer
 * @return          true if both tags were found
 */
bool extractText(const char* xml, const char* startTag, const char* endTag, char* out, size_t cap) {
  const char* s = strstr(xml, startTag);
  if (!s) return false;
  s += strlen(startTag);
  const char* e = strstr(s, endTag);
  if (!e) return false;
  size_t n = e - s;
  if (n >= cap) n = cap - 1;
  memcpy(out, s, n);
  out[n] = '\0';
  return true;
}

/**
 * Like extractText, but converts the result to a number ("100%" gives 100).
 *
 * @param xml       the packet text
 * @param startTag  text just before the number
 * @param endTag    text just after the number
 * @param out       receives the value (NaN if the text between the tags isn't a number);
 *                  untouched if a tag is missing
 * @return          true if both tags were found
 */
bool extractFloat(const char* xml, const char* startTag, const char* endTag, float& out) {
  char tmp[24];
  if (!extractText(xml, startTag, endTag, tmp, sizeof tmp)) return false;
  char* end = nullptr;
  out = strtof(tmp, &end);
  if (end == tmp) out = NAN;                   // no digits at all: don't pass a quiet 0 on
  return true;
}

/**
 * Checks that a number is believable. NaN fails both comparisons and infinity is outside
 * any limit, so neither can get through.
 *
 * @param v   the number
 * @param lo  smallest acceptable value
 * @param hi  largest acceptable value
 * @return    true if v is between lo and hi (inclusive)
 */
bool inRange(float v, float lo, float hi) {
  return v >= lo && v <= hi;
}

/**
 * Logs why a packet was thrown away (debug builds only).
 *
 * @param field  name of the value that was out of range
 * @return       always ParseResult::Rejected
 */
ParseResult rejectPacket(const char* field) {
  (void)field;                                 // only used by the debug log
  DBG("Ignored a packet: %s is not a believable value\n", field);
  return ParseResult::Rejected;
}

/**
 * Updates `latest` from whichever electricity fields this packet contains. Fields that
 * aren't in the packet are left alone. All or nothing: if any field in the packet is not
 * a believable number, `latest` is not changed at all.
 *
 * @param xml  the packet text
 * @return     Accepted if at least one field was found and all were believable,
 *             Rejected if any field was out of range, NotElectricity if there were none
 */
ParseResult parseElectricity(const char* xml) {
  Readings next = latest;                      // only copied back once everything checks out
  bool found = false;
  float v;

  if (extractFloat(xml, "<curr units='w'>", "</curr>", v)) {
    found = true;
    if (!inRange(v, -MAX_WATTS, MAX_WATTS)) return rejectPacket("watts");
    if (v > 0) next.watts = v;                 // the OWL occasionally reports 0, ignore it
  }
  if (extractFloat(xml, "<day units='wh'>", "</day>", v)) {
    found = true;
    if (!inRange(v, -MAX_DAY_WH, MAX_DAY_WH)) return rejectPacket("today's usage");
    if (v > 0) next.kwhToday = v / 1000.0f;
  }
  if (extractFloat(xml, "<battery level='", "'/>", v)) {
    found = true;
    if (!inRange(v, 0.0f, 100.0f)) return rejectPacket("battery");
    next.battery = (int)v;
  }
  if (extractFloat(xml, "</watts><cost>", "</cost></current>", v)) {
    found = true;
    if (!inRange(v, -MAX_COST_NOW_P, MAX_COST_NOW_P)) return rejectPacket("current cost");
    next.costNow = v / 100.0f;                 // pence -> pounds
  }
  if (extractFloat(xml, "</wh><cost>", "</cost></day>", v)) {
    found = true;
    if (!inRange(v, -MAX_COST_DAY_P, MAX_COST_DAY_P)) return rejectPacket("today's cost");
    next.costToday = v / 100.0f;
  }

  if (!found) return ParseResult::NotElectricity;
  latest = next;
  return ParseResult::Accepted;
}

#if DEBUG_SERIAL
/**
 * Logs the weather fields of a packet to Serial (weather isn't shown on screen).
 *
 * @param xml  the packet text
 */
void logWeather(const char* xml) {
  float code, temp;
  char  text[48];
  if (!extractFloat(xml, "code='", "'><temperature>", code)) return;
  if (!extractFloat(xml, "'><temperature>", "</temperature>", temp)) return;
  if (!inRange(code, 0, 10000) || !inRange(temp, -10000, 10000)) return;   // keeps the (int) cast below well-defined
  if (!extractText(xml, "><text>", "</text></w", text, sizeof text)) text[0] = '\0';
  DBG("Weather code: %d  temperature: %.2f  text: %s\n", (int)code, temp / 100.0f, text);
}
#endif

/**
 * Remembers when the latest packet arrived, both as millis() and, if the clock has
 * been set by NTP, as wall-clock time.
 */
void recordPacketTime() {
  struct tm now;
  lastPacketMs = millis();
  if (getLocalTime(&now, 0)) time(&lastPacketEpoch);   // 0 ms: don't wait if the clock isn't set
  else                       lastPacketEpoch = 0;
}

// ----------------------------------------------------------------- network ---

/**
 * Keeps the multicast listener alive. Joins the group once WiFi is up, drops it when
 * WiFi goes away, and retries every LISTEN_RETRY_MS if joining fails. Call every loop.
 *
 * The packet handler runs on the UDP task, so it only copies the packet into a queue;
 * loop() does the parsing and drawing. It drops anything that wasn't sent to the OWL
 * multicast group: the socket is bound to the port on every address, so packets sent
 * straight to this board (unicast or broadcast), or to some other group, would otherwise
 * be accepted too.
 */
void maintainListener() {
  static bool     listening = false;
  static bool     tried     = false;
  static uint32_t lastTry   = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (listening) {                           // WiFi dropped: the group membership is gone
      udp.close();
      listening = false;
    }
    return;
  }
  if (listening) return;
  if (tried && millis() - lastTry < LISTEN_RETRY_MS) return;

  tried   = true;
  lastTry = millis();
  udp.close();
  if (udp.listenMulticast(OWL_GROUP, OWL_PORT)) {
    udp.onPacket([](AsyncUDPPacket packet) {
      static RxPacket rx;                      // only ever used by the UDP task

      if (!packet.isMulticast() || packet.localIP() != OWL_GROUP) {
#if DEBUG_SERIAL
        static bool     noted    = false;      // log this at most once every 10 s, so a flood can't spam Serial
        static uint32_t lastNote = 0;
        if (!noted || millis() - lastNote > 10000UL) {
          noted    = true;
          lastNote = millis();
          DBG("Ignored a packet not addressed to the OWL group (from %s)\n", packet.remoteIP().toString().c_str());
        }
#endif
        return;
      }

      size_t n = packet.length();
      if (n >= sizeof(rx.text)) n = sizeof(rx.text) - 1;
      memcpy(rx.text, packet.data(), n);
      rx.text[n] = '\0';
      xQueueSend(rxQueue, &rx, 0);             // if the queue is full the packet is dropped
    });
    listening = true;
    DBG("Listening for OWL packets on %s:%u\n", OWL_GROUP.toString().c_str(), OWL_PORT);
  } else {
    DBG("Could not join the multicast group, will retry\n");
  }
}

// --------------------------------------------------------------------- OTA ---
#if ENABLE_OTA

/**
 * Puts the normal screen back after an update fails: the dashboard if we already have
 * data, otherwise the waiting screen.
 */
void restoreScreen() {
  if (haveData) {
    layoutDrawn = false;                       // forces a full redraw
    updateDisplay();
  } else {
    showWaitingScreen();
  }
}

/**
 * OTA callback: an update (already authenticated) is starting. Shows the progress screen.
 */
void otaStart() {
  DBG("OTA update starting\n");
  otaLastPercent = 101;
  showSplash("Updating firmware...", "Do not power off", nullptr);
  tft.drawRect(OTA_BAR_X, OTA_BAR_Y, OTA_BAR_W, OTA_BAR_H, WHITE);
}

/**
 * OTA callback: data is arriving. Redraws the bar and percentage only when the
 * percentage changes, so the screen doesn't slow the transfer down.
 *
 * @param done   bytes received so far
 * @param total  size of the update in bytes
 */
void otaProgress(unsigned int done, unsigned int total) {
  const unsigned pct = total ? (unsigned)((uint64_t)done * 100 / total) : 0;
  if (pct == otaLastPercent) return;
  otaLastPercent = pct;

  const int fill = (OTA_BAR_W - 4) * (int)pct / 100;
  if (fill > 0) tft.fillRect(OTA_BAR_X + 2, OTA_BAR_Y + 2, fill, OTA_BAR_H - 4, WHITE);

  char txt[8];
  snprintf(txt, sizeof txt, "%u%%", pct);
  tft.fillRect(OTA_BAR_X + OTA_BAR_W / 2 - 50, OTA_BAR_Y + 28, 100, 28, BLACK);
  printCentred(OTA_BAR_Y + 50, txt);
}

/**
 * OTA callback: the update was written successfully; the board restarts next.
 */
void otaEnd() {
  DBG("OTA update finished\n");
  showSplash("Update complete", "Restarting...", nullptr);
}

/**
 * OTA callback: something went wrong. A wrong password is only logged (showing it on
 * screen would let anyone on the network blank the display). Real failures are shown
 * for a few seconds, then the normal screen comes back.
 *
 * @param error  what failed
 */
void otaError(ota_error_t error) {
  if (error == OTA_AUTH_ERROR) {
    DBG("OTA: update request rejected (wrong password)\n");
    return;
  }

  const char* msg = "Unknown error";
  switch (error) {
    case OTA_BEGIN_ERROR:   msg = "Could not start";   break;
    case OTA_CONNECT_ERROR: msg = "Connection failed"; break;
    case OTA_RECEIVE_ERROR: msg = "Receive failed";    break;
    case OTA_END_ERROR:     msg = "Finishing failed";  break;
    default: break;
  }
  DBG("OTA error: %s\n", msg);
  showSplash("Update failed", msg, nullptr);
  delay(4000);
  restoreScreen();
}

/**
 * Checks that a string is a SHA-256 hash written as 64 hex characters.
 *
 * @param s  the string to check (may be nullptr)
 * @return   true if it is exactly 64 hex digits
 */
bool isSha256Hex(const char* s) {
  if (s == nullptr || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return true;
}

/**
 * Starts OTA updates. config.h holds only the SHA-256 hash of the OTA password
 * (otaPasswordHash); you type the real password into the Arduino IDE when you upload.
 * OTA stays off if the hash is missing, malformed, or the hash of an empty password, so
 * the board can never be left open to anyone on the network.
 */
void setupOTA() {
  // SHA-256 of the empty string, i.e. "no password at all"
  const char* EMPTY_PASSWORD_HASH = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

  if (!isSha256Hex(otaPasswordHash)) {
    Serial.println("OTA disabled: otaPasswordHash in config.h must be the 64-character SHA-256 hex of your OTA password");
    return;
  }
  char hash[65];                               // the OTA library needs lower case hex
  for (int i = 0; i < 64; i++) hash[i] = tolower((unsigned char)otaPasswordHash[i]);
  hash[64] = '\0';
  if (strcmp(hash, EMPTY_PASSWORD_HASH) == 0) {
    Serial.println("OTA disabled: otaPasswordHash is the hash of an empty password");
    return;
  }

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPasswordHash(hash);
  ArduinoOTA.onStart(otaStart);
  ArduinoOTA.onProgress(otaProgress);
  ArduinoOTA.onEnd(otaEnd);
  ArduinoOTA.onError(otaError);
  ArduinoOTA.begin();
  otaEnabled = true;
  Serial.printf("OTA ready: %s.local\n", OTA_HOSTNAME);
}

#endif  // ENABLE_OTA

// ------------------------------------------------------------------- setup ---

/**
 * Runs once at boot: starts the screen, connects to WiFi (restarting if it can't),
 * starts the clock and OTA, and shows the "waiting" screen.
 */
void setup() {
  Serial.begin(115200);

  // Start the screen first so progress and errors are visible
  tft.reset();
  uint16_t ID = tft.readID();
  tft.begin(ID);
  tft.setRotation(1);
  tft.invertDisplay(true);
  tft.setFont(&FreeSans9pt7b);
  showSplash("Connecting to WiFi...", nullptr, nullptr);

  rxQueue = xQueueCreate(6, sizeof(RxPacket));
  if (!rxQueue) {
    Serial.println("Out of memory creating the packet queue - restarting");
    delay(3000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);                      // don't keep a second copy of the credentials in flash
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("WiFi Failed! Restarting...");
    showSplash("WiFi failed!", "Restarting...", nullptr);
    delay(3000);
    ESP.restart();                             // try the connection again from scratch
  }

  Serial.println("Wifi Connected!");
  configTzTime(TIMEZONE, NTP_SERVER);          // clock for the "last packet" time
  showWaitingScreen();
#if ENABLE_OTA
  setupOTA();
#endif
}

// -------------------------------------------------------------------- loop ---

/**
 * Runs forever: services OTA, keeps the listener alive, handles any packets that have
 * arrived and refreshes the screen. A packet that was rejected doesn't count as "heard
 * from the OWL": it doesn't update the last-packet time or the stale-data timer.
 */
void loop() {
#if ENABLE_OTA
  if (otaEnabled) ArduinoOTA.handle();         // an update runs inside this call
#endif
  maintainListener();

  // Handle every packet that has arrived since last time
  static RxPacket rx;
  bool changed = false;
  while (xQueueReceive(rxQueue, &rx, 0) == pdTRUE) {
    DBG("Packet: %s\n", rx.text);
    switch (parseElectricity(rx.text)) {
      case ParseResult::Accepted:
        haveData = true;
        recordPacketTime();
        changed = true;
        break;
      case ParseResult::NotElectricity:
#if DEBUG_SERIAL
        logWeather(rx.text);
#endif
        break;
      case ParseResult::Rejected:              // already logged by parseElectricity
        break;
    }
  }

  if (changed) updateDisplay();
  if (haveData) updateInfoBox();

  delay(20);
}
