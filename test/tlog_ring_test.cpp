// Host test for aog_logger/tlog.h with a strict NOR flash simulator.
//   g++ -std=c++17 -Wall -o /tmp/tlog_test test/tlog_ring_test.cpp && /tmp/tlog_test /tmp/tlog.bin
//   python3 test/tlog_check.py /tmp/tlog.bin
// Writes a flash image with a known record pattern; tlog_check.py decodes it with the real decoder.
#include "../aog_logger/tlog.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const uint32_t NSEC = 12;
static std::vector<uint8_t> flash(NSEC * tlog::SECTOR, 0xFF);
static std::vector<uint8_t> written(NSEC * tlog::SECTOR, 0);   // 1 = programmed since last erase
static int failures = 0;

static bool fRead(uint32_t off, void *dst, uint32_t len) {
  if (off + len > flash.size()) return false;
  memcpy(dst, &flash[off], len); return true;
}
static bool fWrite(uint32_t off, const void *src, uint32_t len) {
  if (off + len > flash.size()) { puts("FAIL write out of range"); failures++; return false; }
  const uint8_t *s = (const uint8_t *)src;
  for (uint32_t i = 0; i < len; i++) {
    if (written[off + i]) { printf("FAIL byte %u written twice\n", off + i); failures++; return false; }
    flash[off + i] &= s[i]; written[off + i] = 1;
  }
  return true;
}
static bool fErase(uint32_t off) {
  if (off % tlog::SECTOR || off >= flash.size()) { puts("FAIL bad erase"); failures++; return false; }
  memset(&flash[off], 0xFF, tlog::SECTOR); memset(&written[off], 0, tlog::SECTOR); return true;
}

static void check(bool ok, const char *what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

int main(int argc, char **argv) {
  tlog::Flash f = { fRead, fWrite, fErase, (uint32_t)flash.size() };
  static tlog::Ring ring;                  // 4 KB buffer: keep it off the stack
  uint32_t now = 1000, counter = 0;

  // Boot 1: empty flash.
  check(ring.begin(f, 1, now), "begin on blank flash");
  check(ring.newestSeq() == 1 && ring.usedSectors() == 1, "first sector seq 1");
  const char *bootTxt = "fw=test boot=1";
  ring.append(now, tlog::CH_BOOT, 0, (const uint8_t *)bootTxt, strlen(bootTxt));

  // Records: payload = "R" + u32 counter + filler up to a varying length (5..254 incl. truncation).
  auto rec = [&](uint32_t t) {
    uint8_t p[300]; p[0] = 'R'; memcpy(p + 1, &counter, 4);
    uint16_t len = 5 + (counter * 37) % 260;               // some > 254 -> truncated
    for (uint16_t i = 5; i < len && i < sizeof(p); i++) p[i] = (uint8_t)(counter + i);
    bool ok = ring.append(t, counter % 6, 26, p, len);
    counter++;
    return ok;
  };

  for (int i = 0; i < 300; i++) { now += 37; if (!rec(now)) { puts("FAIL append"); failures++; } }
  ring.append(now, tlog::CH_MARK, 0, (const uint8_t *)"marker one", 10);
  now += 70000;                                             // dt overflow -> time sync record
  rec(now);
  ring.flush();
  uint32_t seqBoot1 = ring.newestSeq();
  check(seqBoot1 > 1, "boot 1 spans several sectors");

  // Boot 2: recover and continue after the newest sector.
  now = 500;
  check(ring.begin(f, 2, now), "begin after reboot");
  check(ring.newestSeq() == seqBoot1 + 1, "reboot continues sequence");
  for (int i = 0; i < 2500; i++) { now += 23; rec(now); }  // wraps the 12-sector ring several times
  ring.flush();
  check(ring.usedSectors() == NSEC, "ring full after wrap");

  // Boot 3: power cut without flush (last partial page lost is acceptable), short session.
  now = 200;
  ring.begin(f, 3, now);
  for (int i = 0; i < 40; i++) { now += 50; rec(now); }
  ring.flush();
  printf("records written: %u, newest seq %u\n", counter, ring.newestSeq());

  if (argc > 1) {
    FILE *o = fopen(argv[1], "wb");
    fwrite(flash.data(), 1, flash.size(), o); fclose(o);
    FILE *m = fopen((std::string(argv[1]) + ".expect").c_str(), "w");
    fprintf(m, "%u %u\n", counter, ring.newestSeq()); fclose(m);
  }
  printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
