/*
  Private link to the dumb cabin motor node (UDP over WiFi, off the AgIO ports).
  Command / ACK ping-pong:  the ACK doubles as the cabin heartbeat.

    front --CMD--> cabin   (unicast cfg.cabinIp : 3344)
    cabin --ACK--> front   (back to front : 4433)

  Frames (AOG-style, 8 bytes):
    CMD  [0]0x80 [1]0x81 [2]0x7A(front) [3]0xC0 [4]seq [5]pwmLo [6]pwmHi [7]crc(sum 2..6)
    ACK  [0]0x80 [1]0x81 [2]0x7B(cabin) [3]0xC1 [4]seqEcho [5]pwmApplied [6]status [7]crc

  The front node pings CONTINUOUSLY (pwm=0 when idle) so liveness stays valid
  even while disarmed. "Heartbeat alive" = ACK arriving with an advancing seq
  that echoes a recently-sent CMD.  lastHeartbeatMs (in main .ino) drives the LED.
*/

const uint16_t LINK_PORT_LISTEN = 4433;   // front listens here for ACK
const uint16_t LINK_PORT_SEND   = 3344;   // cabin listens here for CMD
const uint8_t  LINK_SRC_FRONT   = 0x7A;
const uint8_t  LINK_SRC_CABIN   = 0x7B;
const uint8_t  LINK_PGN_CMD     = 0xC0;
const uint8_t  LINK_PGN_ACK     = 0xC1;
WiFiUDP  linkUdp;
uint8_t  linkSeq      = 0;                 // last CMD seq sent
int16_t  linkRttMs    = -1;                // measured round-trip (info)
uint32_t cmdSentAtMs[256];                 // send timestamp per seq (for RTT)

void initLink() {
  linkUdp.begin(LINK_PORT_LISTEN);
}

static uint8_t linkCrc(const uint8_t *p) {     // sum of bytes [2..6]
  uint8_t c = 0;
  for (uint8_t i = 2; i <= 6; i++) c += p[i];
  return c;
}

void sendCmd() {
  if (apMode || WiFi.status() != WL_CONNECTED) return;
  linkSeq++;
  uint8_t f[8];
  f[0] = 0x80; f[1] = 0x81;
  f[2] = LINK_SRC_FRONT;
  f[3] = LINK_PGN_CMD;
  f[4] = linkSeq;
  f[5] = (uint8_t)(pwmCommand & 0xFF);         // pwm Lo
  f[6] = (uint8_t)(pwmCommand >> 8);           // pwm Hi
  f[7] = linkCrc(f);

  cmdSentAtMs[linkSeq] = millis();

  IPAddress cabin(cfg.cabinIp[0], cfg.cabinIp[1], cfg.cabinIp[2], cfg.cabinIp[3]);
  linkUdp.beginPacket(cabin, LINK_PORT_SEND);
  linkUdp.write(f, sizeof(f));
  linkUdp.endPacket();
}

void handleLink() {
  int sz = linkUdp.parsePacket();
  if (sz < 8) return;
  uint8_t f[16];
  int n = linkUdp.read(f, sizeof(f));
  if (n < 8) return;
  if (f[0] != 0x80 || f[1] != 0x81) return;
  if (f[2] != LINK_SRC_CABIN || f[3] != LINK_PGN_ACK) return;
  if (f[7] != linkCrc(f)) return;              // bad CRC -> drop

  uint8_t seq = f[4];
  // accept only an ACK echoing a recently-sent CMD (guards stale/duplicate)
  uint32_t sentAt = cmdSentAtMs[seq];
  if (sentAt == 0 || (millis() - sentAt) > 500) return;

  pwmApplied  = (int8_t)f[5];
  cabinStatus = f[6];
  linkRttMs   = (int16_t)(millis() - sentAt);
  lastHeartbeatMs = millis();                  // heartbeat alive -> LED pulse
}
