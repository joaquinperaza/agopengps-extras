/*
  Status LED: onboard NeoPixel of the QT Py ESP32-S3 (PIN_NEOPIXEL, powered via NEOPIXEL_POWER).
  Called every 200 ms from loop().

  NOTE: the live module's colour scheme could not be observed remotely; this is a stand-in.
    blue blink    config AP mode
    red           WiFi down
    yellow        WiFi ok, no AgIO packets for 2 s
    magenta       AgIO ok, cabin motor node not answering
    dim green     ready (cabin heartbeat alive), not steering
    bright green  steering engaged
*/

const uint16_t LED_AOG_TIMEOUT_MS = 2000;
const uint8_t  LED_DIM = 8, LED_BRIGHT = 60;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(PIN_NEOPIXEL, r, g, b);
}

void initStatusLed() {
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, HIGH);
  setLed(LED_DIM, 0, 0);
}

void updateStatusLed() {
  static bool blink = false;
  blink = !blink;

  if (apMode)                          { setLed(0, 0, blink ? LED_BRIGHT : 0); return; }
  if (WiFi.status() != WL_CONNECTED)   { setLed(LED_BRIGHT, 0, 0); return; }
  if (lastAogMs == 0 || millis() - lastAogMs > LED_AOG_TIMEOUT_MS) { setLed(LED_BRIGHT, LED_BRIGHT / 2, 0); return; }
  if (!cabinAlive())                   { setLed(LED_BRIGHT, 0, LED_BRIGHT); return; }
  if (steerEngaged)                    { setLed(0, LED_BRIGHT, 0); return; }
  setLed(0, LED_DIM, 0);
}
