// Rolling packet log on raw NOR flash. Portable: the .ino plugs in the ESP32 "tlog" partition,
// test/tlog_ring_test.cpp plugs in a RAM flash simulator.
#pragma once
#include <stdint.h>
#include <string.h>

/*
  Log format v1 (little-endian). The log area is N sectors of 4096 bytes; erased flash is 0xFF.

  Sector header (16 bytes):
    u32 magic   'TLG1' (0x31474C54)
    u32 seq     +1 for every sector ever opened (newest sector = highest seq)
    u32 boot    logger boot counter when the sector was opened
    u32 t0      logger millis() when the sector was opened
  Records follow back to back and never span sectors:
    u8  len     payload length 0..254; 0xFF = no more records in this sector (erased flash)
    u8  chan    0x00..0x3F port index (+0x40 if the packet was truncated to 254 bytes),
                0xF0.. logger records (below)
    u8  src     last octet of the sender's IPv4 address (0 for logger records)
    u16 dt      ms since the previous record in this sector (first record: since t0)
    u8  payload[len]
  Logger records:
    0xF0 boot info  ASCII "key=value ..." (ports, ip, firmware, settings)
    0xF1 marker     ASCII text typed by the user in the WebUI
    0xF2 stats      binary, once a minute (layout in aog_logger.ino, struct StatsV1)
    0xF3 time sync  u32 absolute millis(); written instead of an overflowing dt
*/

namespace tlog {

const uint32_t SECTOR = 4096, HDR = 16, REC_HDR = 5, MAGIC = 0x31474C54;
const uint8_t  CH_TRUNC = 0x40, CH_BOOT = 0xF0, CH_MARK = 0xF1, CH_STATS = 0xF2, CH_TIME = 0xF3;
const uint8_t  LEN_END = 0xFF;
const uint16_t MAX_PAYLOAD = 254;
const uint32_t FLUSH_BYTES = 256;          // write to flash at least every 256 bytes

struct Flash {                             // offsets are relative to the start of the log area
  bool (*read)(uint32_t off, void *dst, uint32_t len);
  bool (*write)(uint32_t off, const void *src, uint32_t len);   // NOR: only clears bits
  bool (*erase)(uint32_t off);                                   // erases one SECTOR at off
  uint32_t size;
};

struct SectorHdr { uint32_t magic, seq, boot, t0; };

class Ring {
 public:
  // Scan the area and open a fresh sector after the newest one (older boots are kept).
  bool begin(const Flash &f, uint32_t bootCount, uint32_t nowMs) {
    fl = f; nsec = f.size / SECTOR; boot = bootCount; used = 0; seq = 0;
    int32_t newest = -1;
    for (uint32_t i = 0; i < nsec; i++) {
      SectorHdr h;
      if (!fl.read(i * SECTOR, &h, sizeof(h))) continue;
      if (h.magic != MAGIC || h.seq == 0xFFFFFFFFu) continue;
      used++;
      if (newest < 0 || h.seq > seq) { seq = h.seq; newest = (int32_t)i; }
    }
    cur = newest < 0 ? nsec - 1 : (uint32_t)newest;   // openSector() advances to the next one
    return openSector(nowMs);
  }

  // Append one record (payload is truncated to MAX_PAYLOAD). False only on a flash error.
  bool append(uint32_t nowMs, uint8_t chan, uint8_t src, const uint8_t *p, uint16_t len) {
    if (len > MAX_PAYLOAD) { len = MAX_PAYLOAD; if (chan < 0x80) chan |= CH_TRUNC; }
    uint32_t dt = nowMs - lastMs;
    uint32_t need = REC_HDR + len + (dt > 0xFFFF ? REC_HDR + 4 : 0);
    if (wpos + need > SECTOR) {
      if (!openSector(nowMs)) return false;
      dt = 0;
    }
    if (dt > 0xFFFF) {
      uint8_t t[4]; memcpy(t, &nowMs, 4);
      put(CH_TIME, 0, 0, t, 4);
      dt = 0;
    }
    put(chan, src, (uint16_t)dt, p, len);
    lastMs = nowMs;
    return (wpos - flushed >= FLUSH_BYTES) ? flush() : true;
  }

  bool flush() {
    if (wpos == flushed) return true;
    bool ok = fl.write(cur * SECTOR + flushed, buf + flushed, wpos - flushed);
    flushed = wpos;
    return ok;
  }

  // Close the current sector and start the next one (erasing the oldest data there).
  bool openSector(uint32_t nowMs) {
    bool ok = (nsec > 0) && flush();
    if (nsec == 0) return false;
    cur = (cur + 1) % nsec;
    SectorHdr old;
    bool wasUsed = fl.read(cur * SECTOR, &old, sizeof(old)) && old.magic == MAGIC;
    ok = fl.erase(cur * SECTOR) && ok;
    if (!wasUsed && used < nsec) used++;
    seq++;
    memset(buf, 0xFF, SECTOR);
    SectorHdr h = { MAGIC, seq, boot, nowMs };
    memcpy(buf, &h, HDR);
    wpos = HDR; flushed = 0; lastMs = nowMs;
    return flush() && ok;                  // header goes to flash right away
  }

  uint32_t sectors() const     { return nsec; }
  uint32_t usedSectors() const { return used; }
  uint32_t newestSeq() const   { return seq; }
  uint32_t currentSector() const { return cur; }
  uint32_t bytesInSector() const { return wpos; }

 private:
  void put(uint8_t chan, uint8_t src, uint16_t dt, const uint8_t *p, uint16_t len) {
    buf[wpos++] = (uint8_t)len; buf[wpos++] = chan; buf[wpos++] = src;
    buf[wpos++] = dt & 0xFF;    buf[wpos++] = dt >> 8;
    if (len) memcpy(buf + wpos, p, len);
    wpos += len;
  }

  Flash    fl = {};
  uint32_t nsec = 0, cur = 0, seq = 0, boot = 0, used = 0;
  uint32_t wpos = 0, flushed = 0, lastMs = 0;
  uint8_t  buf[SECTOR];                    // RAM image of the sector being written
};

}  // namespace tlog
