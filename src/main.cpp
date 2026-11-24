// CoreInk Journal: an Obsidian daily journal on an M5Stack CoreInk, fully offline.
//
// Screens
//   HOME     weekday, date, title, the six life-area ratings     wheel up/down = previous/next day
//   SUMMARY  the day's highlights                                wheel up/down = page
//   ARTICLE  the whole entry                                     wheel up/down = page
//   STREAK   punch card of the last 13 weeks + streak counts     wheel up/down = previous/next day
// Wheel press: HOME -> SUMMARY -> ARTICLE (in ARTICLE it turns the page); in STREAK it opens the
//   selected day.
// Side (power) button: back to HOME on the same day, from anywhere.
// Top button: toggles between the day you're on and STREAK.
// Holding the wheel up or down keeps going (after 0.4 s; faster after 3 s), in every screen.
// After IDLE_MS without input the device powers off (the e-ink keeps showing the last screen);
// press the side button to turn it on again. It comes back on the same screen, day and page (saved
// to NVS on the way down); a fresh journal from a sync opens on the most recent day.
//
// Data: /journal.tsv on LittleFS, one day per line, oldest first, written by sync/sync_coreink.py:
//   iso \t date \t weekday \t title \t areas \t highlights \t body
//   areas = "Fam:3,Net:2,..." (3 focused, 2 some, 1 neglected, 0 not today)
//   highlights and body are escaped: \\ -> \, \t -> tab, \n -> newline.
//
// Serial protocol (115200 baud, text lines only). The USB link to this unit corrupts bytes now and
// then, so everything is small, checksummed and safe to resend; the PC retries anything that isn't
// answered cleanly. crc = CRC-32 (zlib) as 8 hex digits.
//   PING                    -> PONG <days>
//   B <len>                 -> KB            start a new upload (truncates the temp file)
//   W <off> <crc> <base64>  -> K <off>       write one chunk at <off> (only if its crc matches)
//                           -> N <off>       crc mismatch / bad line: resend
//   E <len> <crc>           -> D <days>      whole file verified and now in use (idempotent)
//                           -> F <reason>    verification failed: start again with B
//   T <epoch> <utcoff> <crc> -> KT          set the RTC to local time (epoch + utcoff seconds)
//                           -> N T           crc covers "<epoch> <utcoff>"; mismatch: resend

#include <M5Unified.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <algorithm>
#include <vector>

// ---------------------------------------------------------------- pins
static constexpr int PIN_UP = 37;     // wheel up    (input-only, external pull-up)
static constexpr int PIN_MID = 38;    // wheel press
static constexpr int PIN_DOWN = 39;   // wheel down
static constexpr int PIN_PWR = 27;    // side power button (readable while running)
static constexpr int PIN_TOP = 5;     // top (EXT) button
static constexpr int PIN_HOLD = 12;   // keep HIGH to stay powered on battery

static constexpr uint32_t IDLE_MS = 10000;      // power off after 10 s without input (it resumes where it was)
static constexpr uint32_t CPU_MHZ = 80;         // plenty for this; about half the current of 240 MHz
static constexpr int FULL_REFRESH_EVERY = 10;   // partial updates between full (ghost-clearing) refreshes

static const char* DATA_PATH = "/journal.tsv";
static const char* TMP_PATH = "/journal.new";

// ---------------------------------------------------------------- data
struct Day {
  String iso, date, weekday, title, areas, highlights, body;
};

static std::vector<uint32_t> offsets;  // byte offset of each line in DATA_PATH
static std::vector<int32_t> dayNums;   // that line's date as days since 1970-01-01 (sorted, like the file)
static constexpr int32_t NO_DAY = INT32_MIN;

// ---------------------------------------------------------------- dates
static const char* MONTH3[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char* WEEKDAY3[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

// Days since 1970-01-01 <-> civil date (H. Hinnant's algorithms).
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civilFromDays(int32_t z, int& y, int& m, int& d) {
  z += 719468;
  int era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = z - era * 146097;
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yoe + era * 400 + (m <= 2);
}

// 1970-01-01 was a Thursday; weeks start on Monday. Journal dates are all after 1970, so no negatives.
static int weekdayOf(int32_t dn) { return (dn + 3) % 7; }  // Mon = 0
static int32_t weekOf(int32_t dn) { return (dn + 3) / 7; }
static int32_t mondayOf(int32_t week) { return week * 7 - 3; }

static int32_t parseIso(const char* s) {  // "YYYY-MM-DD"
  for (int i = 0; i < 10; i++)
    if ((i == 4 || i == 7) ? s[i] != '-' : (s[i] < '0' || s[i] > '9')) return NO_DAY;
  int y = atoi(s), m = atoi(s + 5), d = atoi(s + 8);
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return NO_DAY;
  return daysFromCivil(y, m, d);
}

// Index of the entry for day `dn`, or -1.
static int indexOfDay(int32_t dn) {
  auto it = std::lower_bound(dayNums.begin(), dayNums.end(), dn);
  return (it != dayNums.end() && *it == dn) ? (int)(it - dayNums.begin()) : -1;
}

// Today from the RTC (set by the sync). Without a sane clock, the most recent entry stands in.
static int32_t todayNum() {
  int32_t last = dayNums.empty() ? 0 : dayNums.back();
  auto dt = M5.Rtc.getDateTime();
  if (dt.date.year < 2024 || dt.date.year > 2099) return last;
  return max(last, daysFromCivil(dt.date.year, dt.date.month, dt.date.date));
}

static String unescape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.length()) {
      char n = s[++i];
      out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
    } else {
      out += c;
    }
  }
  return out;
}

static void buildIndex() {
  offsets.clear();
  dayNums.clear();
  File f = LittleFS.open(DATA_PATH, "r");
  if (!f) return;
  uint8_t buf[512];
  uint32_t pos = 0;
  bool lineStart = true;
  char iso[11];
  int isoLen = -1;  // collecting the line's first 10 chars (its ISO date) while >= 0
  while (true) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      if (lineStart && buf[i] != '\n') {
        offsets.push_back(pos + i);
        dayNums.push_back(NO_DAY);
        lineStart = false;
        isoLen = 0;
      }
      if (buf[i] == '\n') {
        lineStart = true;
        isoLen = -1;
      } else if (isoLen >= 0) {
        iso[isoLen++] = buf[i];
        if (isoLen == 10) { iso[10] = 0; dayNums.back() = parseIso(iso); isoLen = -1; }
      }
    }
    pos += n;
  }
  f.close();
}

static bool loadDay(int i, Day& d) {
  if (i < 0 || i >= (int)offsets.size()) return false;
  File f = LittleFS.open(DATA_PATH, "r");
  if (!f) return false;
  f.seek(offsets[i]);
  String line = f.readStringUntil('\n');
  f.close();
  String parts[7];
  int field = 0;
  int start = 0;
  for (int k = 0; k <= (int)line.length() && field < 7; k++) {
    if (k == (int)line.length() || line[k] == '\t') {
      parts[field++] = line.substring(start, k);
      start = k + 1;
    }
  }
  d.iso = parts[0];
  d.date = parts[1];
  d.weekday = parts[2];
  d.title = unescape(parts[3]);
  d.areas = parts[4];
  d.highlights = unescape(parts[5]);
  d.body = unescape(parts[6]);
  return true;
}

// ---------------------------------------------------------------- layout helpers
enum View { HOME, SUMMARY, ARTICLE, STREAK };

struct Line {
  String text;
  int indent;     // px
  bool bullet;    // draw a dot before this line
};

static M5GFX& gfx = M5.Display;
static const int W = 200, H = 200, MARGIN = 6;

// Word-wrap `text` (may contain newlines) to `width` px in the current font.
// Lines starting with "- " become bullets with a hanging indent.
static void wrapText(const String& text, int width, std::vector<Line>& out, bool blankBetweenParagraphs) {
  int from = 0;
  while (from <= (int)text.length()) {
    int nl = text.indexOf('\n', from);
    if (nl < 0) nl = text.length();
    String para = text.substring(from, nl);
    from = nl + 1;
    para.trim();
    if (para.length() == 0) {
      if (blankBetweenParagraphs && !out.empty() && out.back().text.length()) out.push_back({"", 0, false});
      if (nl >= (int)text.length()) break;
      continue;
    }
    bool bullet = para.startsWith("- ") || para.startsWith("* ");
    if (bullet) para = para.substring(2);
    int indent = bullet ? 12 : 0;
    int avail = width - indent;
    String cur;
    bool firstLine = true;
    int i = 0;
    while (i < (int)para.length()) {
      int sp = para.indexOf(' ', i);
      if (sp < 0) sp = para.length();
      String word = para.substring(i, sp);
      i = sp + 1;
      String trial = cur.length() ? cur + " " + word : word;
      if (gfx.textWidth(trial) <= avail) {
        cur = trial;
        continue;
      }
      if (cur.length()) {
        out.push_back({cur, indent, bullet && firstLine});
        firstLine = false;
      }
      // A single word wider than the line: break it by characters.
      while (gfx.textWidth(word) > avail && word.length() > 1) {
        int k = word.length() - 1;
        while (k > 1 && gfx.textWidth(word.substring(0, k)) > avail) k--;
        out.push_back({word.substring(0, k), indent, bullet && firstLine});
        firstLine = false;
        word = word.substring(k);
      }
      cur = word;
    }
    if (cur.length()) out.push_back({cur, indent, bullet && firstLine});
    if (nl >= (int)text.length()) break;
  }
  while (!out.empty() && out.back().text.length() == 0) out.pop_back();
}

// ---------------------------------------------------------------- state
static int dayIndex = 0;
static View view = HOME;
static int page = 0;
static int pageCount = 1;
static int partials = 0;
static Day day;
static bool dayLoaded = false;
static std::vector<Line> pageLines;   // wrapped text for SUMMARY / ARTICLE
static uint32_t lastInput = 0;
static int32_t cursorDay = 0;     // STREAK: the selected day
static int32_t gridEndWeek = 0;   // STREAK: the week shown in the rightmost column

static const int TEXT_TOP = 26, TEXT_BOTTOM = 184, LINE_H = 18;
static int linesPerPage() { return (TEXT_BOTTOM - TEXT_TOP) / LINE_H; }

static void beginFrame() {
  gfx.startWrite();
  gfx.fillScreen(TFT_WHITE);
  gfx.setTextColor(TFT_BLACK, TFT_WHITE);
  gfx.setTextDatum(top_left);
}

static bool repeating = false;  // the wheel is being held: keep frames fast, no ghost-clearing flash

static void endFrame(bool full) {
  // Full refreshes clear e-ink ghosting; partial ones are fast (~0.3 s). While the wheel is held the
  // full refresh waits for the first frame after it's let go.
  if (full || (++partials >= FULL_REFRESH_EVERY && !repeating)) {
    gfx.setEpdMode(epd_mode_t::epd_quality);
    partials = 0;
  } else {
    gfx.setEpdMode(epd_mode_t::epd_fastest);
  }
  gfx.endWrite();
  gfx.display();
  gfx.waitDisplay();
}

static void drawHeader(const String& left, const String& right) {
  gfx.setFont(&fonts::FreeSansBold9pt7b);
  gfx.drawString(left, MARGIN, 4);
  gfx.setFont(&fonts::Font0);
  gfx.setTextDatum(top_right);
  gfx.drawString(right, W - MARGIN, 8);
  gfx.setTextDatum(top_left);
  gfx.drawFastHLine(0, 22, W, TFT_BLACK);
}

static void drawFooter(const String& text) {
  gfx.drawFastHLine(0, 188, W, TFT_BLACK);
  gfx.setFont(&fonts::Font0);
  gfx.setTextDatum(top_right);
  gfx.drawString(text, W - MARGIN, 191);
  gfx.setTextDatum(top_left);
}

// Six life areas: a 3-letter label over a 3-bar meter (3 focused, 2 some, 1 neglected, 0 not today).
static void drawAreas(const String& areas, int y) {
  std::vector<String> labels;
  std::vector<int> levels;
  int from = 0;
  while (from < (int)areas.length()) {
    int comma = areas.indexOf(',', from);
    if (comma < 0) comma = areas.length();
    String item = areas.substring(from, comma);
    from = comma + 1;
    int colon = item.indexOf(':');
    if (colon <= 0) continue;
    labels.push_back(item.substring(0, colon));
    levels.push_back(item.substring(colon + 1).toInt());
  }
  if (labels.empty()) return;
  int cell = W / labels.size();
  gfx.setFont(&fonts::Font0);
  gfx.setTextDatum(top_center);
  for (size_t i = 0; i < labels.size(); i++) {
    int cx = i * cell + cell / 2;
    gfx.drawString(labels[i], cx, y);
    for (int b = 0; b < 3; b++) {
      int h = 4 + b * 4;
      int x = cx - 9 + b * 7;
      int top = y + 24 - h;
      if (b < levels[i]) gfx.fillRect(x, top, 5, h, TFT_BLACK);
      else gfx.drawRect(x, top, 5, h, TFT_BLACK);
    }
  }
  gfx.setTextDatum(top_left);
}

static void drawEmpty() {
  beginFrame();
  gfx.setFont(&fonts::FreeSansBold9pt7b);
  gfx.drawString("No journal yet", MARGIN, 60);
  gfx.setFont(&fonts::FreeSans9pt7b);
  gfx.drawString("Plug in USB and", MARGIN, 90);
  gfx.drawString("run Sync on the PC.", MARGIN, 110);
  endFrame(true);
}

static void drawHome(bool full) {
  beginFrame();
  String weekday = day.weekday;
  weekday.toUpperCase();
  drawHeader(weekday, String(dayIndex + 1) + "/" + String(offsets.size()));

  gfx.setFont(&fonts::FreeSansBold12pt7b);
  gfx.drawString(day.date, MARGIN, 30);
  gfx.drawFastHLine(MARGIN, 60, W - 2 * MARGIN, TFT_BLACK);

  // Title: as many wrapped lines as fit above the area meters; the last one gets "..." if cut.
  gfx.setFont(&fonts::FreeSans12pt7b);
  std::vector<Line> lines;
  wrapText(day.title, W - 2 * MARGIN, lines, false);
  const int titleLineH = 25, maxLines = 4;
  for (int i = 0; i < (int)lines.size() && i < maxLines; i++) {
    String t = lines[i].text;
    if (i == maxLines - 1 && (int)lines.size() > maxLines) {
      while (t.length() > 1 && gfx.textWidth(t + "...") > W - 2 * MARGIN) t.remove(t.length() - 1);
      t += "...";
    }
    gfx.drawString(t, MARGIN, 66 + i * titleLineH);
  }

  gfx.drawFastHLine(0, 168, W, TFT_BLACK);
  drawAreas(day.areas, 172);
  endFrame(full);
}

static void prepareText() {
  pageLines.clear();
  gfx.setFont(&fonts::FreeSans9pt7b);
  if (view == SUMMARY) {
    wrapText(day.highlights.length() ? day.highlights : String("No highlights for this day."), W - 2 * MARGIN,
             pageLines, false);
  } else {
    wrapText(day.body.length() ? day.body : String("This day has no text."), W - 2 * MARGIN, pageLines, true);
  }
  int per = linesPerPage();
  pageCount = max(1, (int)((pageLines.size() + per - 1) / per));
  page = constrain(page, 0, pageCount - 1);
}

static void drawText(bool full) {
  beginFrame();
  String shortDate = day.date.substring(0, day.date.lastIndexOf(' '));  // "22 Nov"
  drawHeader(view == SUMMARY ? "Summary" : "Entry", shortDate);
  gfx.setFont(&fonts::FreeSans9pt7b);
  int per = linesPerPage();
  for (int i = 0; i < per; i++) {
    int k = page * per + i;
    if (k >= (int)pageLines.size()) break;
    const Line& ln = pageLines[k];
    int y = TEXT_TOP + i * LINE_H;
    if (ln.bullet) gfx.fillCircle(MARGIN + 3, y + 7, 2, TFT_BLACK);
    gfx.drawString(ln.text, MARGIN + ln.indent, y);
  }
  drawFooter(String(page + 1) + "/" + String(pageCount));
  endFrame(full);
}

// ---------------------------------------------------------------- streak
// A punch card like GitHub's: one column per week (Mon at the top), the newest week on the right.
static const int COLS = 13, CELL = 12, PITCH = 14, GRID_X = 16, GRID_Y = 38;

struct Streaks { int current, longest, total; };

static Streaks computeStreaks(int32_t today) {
  Streaks s{0, 0, (int)dayNums.size()};
  int run = 0;
  int32_t prev = NO_DAY;
  for (int32_t dn : dayNums) {
    if (dn == NO_DAY) continue;
    run = (prev != NO_DAY && dn == prev + 1) ? run + 1 : 1;
    s.longest = max(s.longest, run);
    prev = dn;
  }
  // No entry yet today: the streak up to yesterday still counts.
  int32_t d = indexOfDay(today) >= 0 ? today : today - 1;
  while (indexOfDay(d) >= 0) { s.current++; d--; }
  return s;
}

static void keepCursorVisible() {
  int32_t w = weekOf(cursorDay);
  if (w > gridEndWeek) gridEndWeek = w;
  if (w < gridEndWeek - (COLS - 1)) gridEndWeek = w + COLS - 1;
}

static void drawStat(int cx, int value, const char* label) {
  gfx.setTextDatum(top_center);
  gfx.setFont(&fonts::FreeSansBold12pt7b);
  gfx.drawString(String(value), cx, 142);
  gfx.setFont(&fonts::Font0);
  gfx.drawString(label, cx, 170);
  gfx.setTextDatum(top_left);
}

static void drawStreak(bool full) {
  int32_t today = todayNum();
  int32_t first = dayNums.front();
  beginFrame();

  int y, m, d;
  civilFromDays(cursorDay, y, m, d);
  int idx = indexOfDay(cursorDay);
  drawHeader(String(WEEKDAY3[weekdayOf(cursorDay)]) + " " + d + " " + MONTH3[m - 1] + " " + y,
             idx >= 0 ? String(idx + 1) + "/" + String(offsets.size()) : String("no entry"));

  // Month names over the column holding each month's 1st.
  gfx.setFont(&fonts::Font0);
  int32_t firstWeek = gridEndWeek - (COLS - 1);
  for (int c = 0; c < COLS; c++) {
    int32_t mon = mondayOf(firstWeek + c);
    for (int r = 0; r < 7; r++) {
      int yy, mm, dd;
      civilFromDays(mon + r, yy, mm, dd);
      if (dd == 1) gfx.drawString(MONTH3[mm - 1], min(GRID_X + c * PITCH, W - 19), 27);
    }
  }
  gfx.drawString("M", 4, GRID_Y + 0 * PITCH + 2);
  gfx.drawString("W", 4, GRID_Y + 2 * PITCH + 2);
  gfx.drawString("F", 4, GRID_Y + 4 * PITCH + 2);

  // Filled = journaled, outline = missed; nothing before the first entry or after today.
  for (int c = 0; c < COLS; c++) {
    int32_t mon = mondayOf(firstWeek + c);
    for (int r = 0; r < 7; r++) {
      int32_t dn = mon + r;
      if (dn < first || dn > today) continue;
      int x = GRID_X + c * PITCH, yy = GRID_Y + r * PITCH;
      bool has = indexOfDay(dn) >= 0;
      if (dn == cursorDay) {
        // Selected: a thick ring, with a block in the middle if that day has an entry.
        gfx.fillRect(x - 2, yy - 2, CELL + 4, CELL + 4, TFT_BLACK);
        gfx.fillRect(x + 1, yy + 1, CELL - 2, CELL - 2, TFT_WHITE);
        if (has) gfx.fillRect(x + 3, yy + 3, CELL - 6, CELL - 6, TFT_BLACK);
      } else if (has) {
        gfx.fillRect(x, yy, CELL, CELL, TFT_BLACK);
      } else {
        gfx.drawRect(x, yy, CELL, CELL, TFT_BLACK);
      }
    }
  }

  Streaks s = computeStreaks(today);
  drawStat(33, s.current, "STREAK");
  drawStat(100, s.longest, "LONGEST");
  drawStat(167, s.total, "DAYS");

  drawFooter("wheel: day -/+");
  if (idx >= 0) gfx.drawString("press: open", MARGIN, 191);
  endFrame(full);
}

static void enterStreak() {
  if (dayNums.empty()) return;
  int32_t today = todayNum();
  view = STREAK;
  // Start on the day you were reading.
  cursorDay = dayIndex < (int)dayNums.size() && dayNums[dayIndex] != NO_DAY ? dayNums[dayIndex] : today;
  gridEndWeek = weekOf(today);
  if (weekOf(cursorDay) < gridEndWeek - (COLS - 1)) gridEndWeek = weekOf(cursorDay) + COLS / 2;
  drawStreak(false);
}

static void moveCursor(int delta) {
  int32_t first = dayNums.front() != NO_DAY ? dayNums.front() : cursorDay;
  int32_t next = constrain(cursorDay + delta, first, todayNum());
  if (next == cursorDay) return;
  cursorDay = next;
  keepCursorVisible();
  drawStreak(false);
}

static void render(bool full) {
  if (offsets.empty()) return drawEmpty();
  if (view == STREAK) return drawStreak(full);
  if (!dayLoaded) {
    dayLoaded = loadDay(dayIndex, day);
    if (!dayLoaded) return drawEmpty();
  }
  if (view == HOME) drawHome(full);
  else drawText(full);
}

static void showDay(int i, bool full) {
  dayIndex = constrain(i, 0, max(0, (int)offsets.size() - 1));
  dayLoaded = false;
  view = HOME;
  page = 0;
  render(full);
}

// ---------------------------------------------------------------- buttons
static constexpr uint32_t REPEAT_DELAY = 400;      // hold this long before the wheel starts repeating
static constexpr uint32_t REPEAT_EVERY = 150;      // then one step per this many ms...
static constexpr uint32_t REPEAT_FAST_AFTER = 3000; // ...and after this long held,
static constexpr uint32_t REPEAT_EVERY_FAST = 40;   // one step per this many ms

struct Btn {
  int pin;
  bool down = false;
  uint32_t changed = 0;
  uint32_t nextRepeat = 0;
  explicit Btn(int p) : pin(p) {}
  // True once per press (on the falling edge, debounced). Buttons pull the pin LOW.
  bool pressed() {
    bool now = digitalRead(pin) == LOW;
    uint32_t t = millis();
    if (now != down && t - changed > 30) {
      down = now;
      changed = t;
      nextRepeat = t + REPEAT_DELAY;
      return now;
    }
    return false;
  }
  // While held past REPEAT_DELAY: the number of steps due since the last call. Counted from the clock,
  // so the pace stays the same however long each e-ink refresh takes (a slow frame just moves further).
  int repeats() {
    if (!down || digitalRead(pin) != LOW) return 0;
    uint32_t t = millis();
    if ((int32_t)(t - nextRepeat) < 0) return 0;
    uint32_t every = t - changed > REPEAT_FAST_AFTER ? REPEAT_EVERY_FAST : REPEAT_EVERY;
    int n = 1 + (t - nextRepeat) / every;
    nextRepeat += n * every;
    return n;
  }
};

static Btn btnUp{PIN_UP}, btnMid{PIN_MID}, btnDown{PIN_DOWN}, btnPwr{PIN_PWR}, btnTop{PIN_TOP};

// Wheel up/down by `n` steps (n > 1 when held down and catching up).
static void onUp(int n = 1) {
  if (view == STREAK) {
    moveCursor(-n);  // up the column, then on to the previous week's Sunday
  } else if (view == HOME) {
    if (dayIndex > 0) showDay(dayIndex - n, false);
  } else if (page > 0) {
    page = max(0, page - n);
    drawText(false);
  }
}

static void onDown(int n = 1) {
  if (view == STREAK) {
    moveCursor(n);   // down the column, then on to the next week's Monday
  } else if (view == HOME) {
    if (dayIndex + 1 < (int)offsets.size()) showDay(dayIndex + n, false);
  } else if (page + 1 < pageCount) {
    page = min(pageCount - 1, page + n);
    drawText(false);
  }
}

static void onMid() {
  if (offsets.empty()) return;
  if (view == STREAK) {
    int idx = indexOfDay(cursorDay);
    if (idx >= 0) showDay(idx, false);  // a missed day has nothing to open
  } else if (view == HOME) {
    view = SUMMARY;
    page = 0;
    prepareText();
    drawText(false);
  } else if (view == SUMMARY) {
    view = ARTICLE;
    page = 0;
    prepareText();
    drawText(false);
  } else {
    onDown();  // in the entry, a press turns the page too
  }
}

static void onBack() {
  if (offsets.empty()) return;
  view = HOME;
  page = 0;
  render(false);
}

static void onTop() {
  if (offsets.empty()) return;
  if (view == STREAK) {
    view = HOME;  // back to the day you came from
    page = 0;
    render(false);
  } else {
    enterStreak();
  }
}

// ---------------------------------------------------------------- power
// What's on screen, kept in NVS across power-offs so a wake carries on from there.
static void saveState() {
  Preferences p;
  if (!p.begin("ui", false)) return;
  p.putUChar("view", (uint8_t)view);
  p.putInt("day", dayIndex);
  p.putInt("page", page);
  p.putInt("cursor", cursorDay);
  p.putInt("gridEnd", gridEndWeek);
  p.putUInt("days", offsets.size());  // a different journal makes the rest meaningless
  p.end();
}

static bool restoreState() {
  if (offsets.empty()) return false;
  Preferences p;
  if (!p.begin("ui", true)) return false;
  bool ok = p.getUInt("days", 0) == offsets.size() && p.getUChar("view", 255) <= STREAK;
  if (ok) {
    view = (View)p.getUChar("view", HOME);
    dayIndex = constrain(p.getInt("day", 0), 0, (int)offsets.size() - 1);
    page = max(0, p.getInt("page", 0));
    cursorDay = p.getInt("cursor", 0);
    gridEndWeek = p.getInt("gridEnd", 0);
  }
  p.end();
  if (!ok) return false;
  dayLoaded = loadDay(dayIndex, day);
  if (view == SUMMARY || view == ARTICLE) prepareText();  // also clamps the page
  if (view == STREAK) {
    if (dayNums.front() == NO_DAY) {
      view = HOME;
    } else {
      cursorDay = constrain(cursorDay, dayNums.front(), todayNum());
      keepCursorVisible();
    }
  }
  return true;
}

static void powerOff() {
  // The e-ink keeps the last image. On battery, releasing HOLD cuts power; on USB power the chip
  // stays up, so it deep-sleeps instead and wakes on a wheel press.
  saveState();
  Serial.println("SLEEP");
  Serial.flush();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_38, 0);
  digitalWrite(PIN_HOLD, LOW);
  delay(300);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------- serial sync
static uint32_t crcTable[256];

static void crcInit() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[i] = c;
  }
}

static uint32_t crcUpdate(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) crc = crcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static File upload;          // temp file being written
static uint32_t uploadLen = 0;

static uint32_t fileCrc(const char* path, uint32_t& size) {
  File f = LittleFS.open(path, "r");
  size = 0;
  if (!f) return 0;
  static uint8_t buf[512];
  uint32_t crc = 0;
  while (true) {
    int n = f.read(buf, sizeof buf);
    if (n <= 0) break;
    crc = crcUpdate(crc, buf, n);
    size += n;
  }
  f.close();
  return crc;
}

static void cmdBegin(uint32_t len) {
  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  File old = LittleFS.open(DATA_PATH, "r");
  if (old) { freeBytes += old.size(); old.close(); }
  if (upload) upload.close();
  LittleFS.remove(TMP_PATH);
  if (len + 8192 > freeBytes) { Serial.printf("F space %u %u\n", (unsigned)len, (unsigned)freeBytes); return; }
  upload = LittleFS.open(TMP_PATH, "w");
  if (!upload) { Serial.println("F open"); return; }
  uploadLen = len;
  Serial.println("KB");
}

static void cmdWrite(const String& line) {
  // W <off> <crc> <base64>
  int s1 = line.indexOf(' ', 2), s2 = s1 < 0 ? -1 : line.indexOf(' ', s1 + 1);
  if (s1 < 0 || s2 < 0 || !upload) { Serial.println("N -1"); return; }
  uint32_t off = strtoul(line.substring(2, s1).c_str(), nullptr, 10);
  uint32_t want = strtoul(line.substring(s1 + 1, s2).c_str(), nullptr, 16);
  static uint8_t buf[1100];
  size_t n = 0;
  const char* b64 = line.c_str() + s2 + 1;
  int rc = mbedtls_base64_decode(buf, sizeof buf, &n, (const unsigned char*)b64, strlen(b64));
  // Only append at the current end, or rewrite at an earlier offset (a resend after a lost reply).
  if (rc != 0 || crcUpdate(0, buf, n) != want || off > upload.size() || off + n > uploadLen) {
    Serial.printf("N %u\n", (unsigned)off);
    return;
  }
  upload.seek(off);
  upload.write(buf, n);
  upload.flush();
  Serial.printf("K %u\n", (unsigned)off);
}

static void cmdEnd(uint32_t len, uint32_t want) {
  if (upload) upload.close();
  uint32_t size = 0;
  if (LittleFS.exists(TMP_PATH)) {
    uint32_t crc = fileCrc(TMP_PATH, size);
    if (size != len || crc != want) { Serial.printf("F crc %u %08x\n", (unsigned)size, (unsigned)crc); return; }
    LittleFS.remove(DATA_PATH);
    LittleFS.rename(TMP_PATH, DATA_PATH);
    buildIndex();
    Serial.printf("D %u\n", (unsigned)offsets.size());
    showDay(offsets.size() - 1, true);  // most recent day, with a clean full refresh
    return;
  }
  // Already applied (the PC didn't hear our D): confirm from the live file.
  uint32_t crc = fileCrc(DATA_PATH, size);
  if (size == len && crc == want) Serial.printf("D %u\n", (unsigned)offsets.size());
  else Serial.println("F missing");
}

static void cmdTime(const String& line) {
  // T <epoch> <utcoff> <crc>
  int s1 = line.indexOf(' ', 2), s2 = s1 < 0 ? -1 : line.indexOf(' ', s1 + 1);
  if (s1 < 0 || s2 < 0) { Serial.println("N T"); return; }
  String signedPart = line.substring(2, s2);
  uint32_t want = strtoul(line.substring(s2 + 1).c_str(), nullptr, 16);
  if (crcUpdate(0, (const uint8_t*)signedPart.c_str(), signedPart.length()) != want) { Serial.println("N T"); return; }
  time_t local = (time_t)(strtoll(line.substring(2, s1).c_str(), nullptr, 10) +
                          strtol(line.substring(s1 + 1, s2).c_str(), nullptr, 10));
  struct tm t;
  gmtime_r(&local, &t);
  M5.Rtc.setDateTime(&t);
  Serial.println("KT");
}

static String cmd;

static void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (cmd.length() < 2000) cmd += c;
      continue;
    }
    String line = cmd;
    cmd = "";
    line.trim();
    lastInput = millis();
    if (line == "PING") {
      Serial.printf("PONG %u\n", (unsigned)offsets.size());
    } else if (line.startsWith("W ")) {
      cmdWrite(line);
    } else if (line.startsWith("T ")) {
      cmdTime(line);
    } else if (line.startsWith("B ")) {
      cmdBegin(strtoul(line.c_str() + 2, nullptr, 10));
    } else if (line.startsWith("E ")) {
      int sp = line.indexOf(' ', 2);
      if (sp > 0) cmdEnd(strtoul(line.substring(2, sp).c_str(), nullptr, 10),
                         strtoul(line.substring(sp + 1).c_str(), nullptr, 16));
    }
    // Anything else is noise: ignore it silently.
  }
}

// ---------------------------------------------------------------- main
void setup() {
  pinMode(PIN_HOLD, OUTPUT);
  digitalWrite(PIN_HOLD, HIGH);  // stay on after the power button is released
  setCpuFrequencyMhz(CPU_MHZ);

  Serial.setRxBufferSize(4096);
  Serial.begin(115200);

  auto cfg = M5.config();
  M5.begin(cfg);
  gfx.setRotation(0);
  gfx.setAutoDisplay(false);

  pinMode(PIN_UP, INPUT);
  pinMode(PIN_MID, INPUT);
  pinMode(PIN_DOWN, INPUT);
  pinMode(PIN_PWR, INPUT_PULLUP);
  pinMode(PIN_TOP, INPUT_PULLUP);

  crcInit();
  if (!LittleFS.begin(true)) Serial.println("ERR littlefs");
  buildIndex();
  Serial.printf("COREINK-JOURNAL READY 2 %u\n", (unsigned)offsets.size());

  // Back where it was when it powered off; otherwise the most recent day.
  if (restoreState()) render(true);
  else showDay(offsets.empty() ? 0 : offsets.size() - 1, true);

  // The power button is usually still held from switching on: don't read that as "back".
  btnPwr.down = digitalRead(PIN_PWR) == LOW;
  btnMid.down = digitalRead(PIN_MID) == LOW;
  btnTop.down = digitalRead(PIN_TOP) == LOW;
  lastInput = millis();
}

void loop() {
  pollSerial();
  bool any = false;
  if (btnUp.pressed()) { onUp(); any = true; }
  if (btnDown.pressed()) { onDown(); any = true; }
  if (btnMid.pressed()) { onMid(); any = true; }
  if (btnPwr.pressed()) { onBack(); any = true; }
  if (btnTop.pressed()) { onTop(); any = true; }
  // Holding the wheel up or down keeps moving.
  int up = btnUp.repeats(), dn = btnDown.repeats();
  repeating = up || dn;
  if (up) { onUp(up); any = true; }
  if (dn) { onDown(dn); any = true; }
  if (any) lastInput = millis();
  if (millis() - lastInput > IDLE_MS) powerOff();
  delay(5);
}
