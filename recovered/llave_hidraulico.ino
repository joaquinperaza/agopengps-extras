#include <WiFi.h>
#include <WiFiUdp.h>

// --- ESP32-C3 Button Pins ---
// Using safe, general-purpose pins for the ESP32-C3
const int BTN_PINS[5] = {6, 3, 2, 5, 4}; 

// --- State tracking for debouncing ---
bool lastFlickerableState[5] = {HIGH, HIGH, HIGH, HIGH, HIGH}; // Tracks raw pin noise
bool currentBtnState[5] = {HIGH, HIGH, HIGH, HIGH, HIGH};      // Tracks confirmed stable state
unsigned long lastDebounceTime[5] = {0, 0, 0, 0, 0};
const unsigned long debounceDelay = 50; // 50ms debounce

// --- Network Settings ---
const char* primarySsid = "AGOPENGPS";
const char* primaryPass = "agopengps"; 

const char* fallbackSsid = "HIDRAULICO_SINSENAL";
// Open network, so no password variable is needed for the fallback

// Variables to track which network was chosen during setup
const char* activeSsid = nullptr;

// --- UDP Settings ---
WiFiUDP udp;
const char* broadcastIP = "192.168.0.255"; // Broadcast address for 192.168.0.x
const int udpPort = 4242;

void setup() {
  Serial.begin(115200);
  
  // 1. Initialize Buttons with Internal Pullups
  for (int i = 0; i < 5; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }

  Serial.println("\n--- WiFi Initialization ---");
  WiFi.mode(WIFI_STA);
  
  // 2. Attempt Primary Network
  Serial.print("Trying Primary Network: ");
  Serial.println(primarySsid);
  WiFi.begin(primarySsid, primaryPass);

  // Wait up to 10 seconds (20 attempts * 500ms) for primary network
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(800);
    Serial.print(".");
    attempts++;
  }

  // 3. Evaluate Connection and Try Fallback if Necessary
  if (WiFi.status() == WL_CONNECTED) {
    activeSsid = primarySsid;
    Serial.println("\nConnected to Primary Network!");
  } else {
    Serial.println("\nPrimary failed. Switching to Fallback (Open): ");
    Serial.println(fallbackSsid);
    
    // Stop trying the primary network
    WiFi.disconnect();
    delay(100);
    
    // Begin fallback connection (Open network, so no password argument)
    WiFi.begin(fallbackSsid); 
    
    // Wait indefinitely for fallback to connect
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    activeSsid = fallbackSsid;
    Serial.println("\nConnected to Fallback Network!");
  }

  // 4. Force ESP32 to auto-reconnect to the actively chosen network if signal drops
  WiFi.setAutoReconnect(true);

  Serial.print("Active SSID Locked In: ");
  Serial.println(activeSsid);
  Serial.print("Remote IP Address: ");
  Serial.println(WiFi.localIP());
}

void sendCommand(uint8_t cmd) {
  // Construct the 4-byte packet
  uint8_t packet[4];
  packet[0] = 0x3A; // 58 in Decimal
  packet[1] = 0x3A; // 58 in Decimal
  packet[2] = cmd;  // 0x00 to 0x04
  packet[3] = (packet[0] + packet[1] + packet[2]) & 0xFF; // CRC calculation

  // Send two packets for redundancy as requested
  for (int i = 0; i < 2; i++) {
    udp.beginPacket(broadcastIP, udpPort);
    udp.write(packet, 4);
    udp.endPacket();
    Serial.printf("Packet %d sent for Command: 0x%02X\n", i + 1, cmd);
    delay(100); // Brief pause between redundant packets
  }
}

void loop() {
  // Only process buttons if we are connected to the network
  // If connection drops, auto-reconnect handles establishing the connection in the background
  if (WiFi.status() != WL_CONNECTED) {
    return; 
  }

  // Iterate through all 5 buttons
  for (int i = 0; i < 5; i++) {
    bool reading = digitalRead(BTN_PINS[i]);

    // If the raw switch reading changed (due to noise or pressing)
    if (reading != lastFlickerableState[i]) {
      lastDebounceTime[i] = millis(); // Reset the debounce timer
    }

    // If the reading has been stable for longer than the debounce delay
    if ((millis() - lastDebounceTime[i]) > debounceDelay) {
      
      // If this stable reading is different from our confirmed state
      if (reading != currentBtnState[i]) {
        currentBtnState[i] = reading; // Lock in the new stable state

        // If the new stable state is LOW (Button Pressed)
        if (currentBtnState[i] == LOW) {
          Serial.printf("Button %d pressed!\n", i);
          sendCommand(i); 
        }
      }
    }
    
    // Save the raw reading for the next loop's noise detection
    lastFlickerableState[i] = reading;
  }
}