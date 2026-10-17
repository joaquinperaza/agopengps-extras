#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WiFiUdp.h>

// --- Relay Pins (Based on LILYGO T-Relay) ---
#define RELAY_1 21
#define RELAY_2 19
#define RELAY_3 18
#define RELAY_4 5

// --- Network Settings ---
const char* ssid1 = "AGOPENGPS";
const char* password = "agopengps"; // Add password if required
const char* apSSID = "HIDRAULICO_SINSENAL";

IPAddress local_IP(192, 168, 0, 42);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);

// --- Server & UDP ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
WiFiUDP udp;
const int udpPort = 4242;

// --- State Variables ---
// 0: Levante I, 1: Levante D, 2: Apertura I, 3: Apertura D (All OFF), 4: Backup
uint8_t currentState = 3; 

// --- Timeout Variables ---
unsigned long lastInteractionTime = 0;
const unsigned long INACTIVITY_TIMEOUT = 120000; // 60 seconds in milliseconds

// --- HTML/CSS/JS UI (Modern & Clean) ---
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Control Panel</title>
  <style>
    body {
      font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
      background-color: #1a1a2e;
      color: #ffffff;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      min-height: 100vh;
      margin: 0;
    }
    h2 { margin-bottom: 30px; letter-spacing: 2px; text-transform: uppercase; color: #0f3460; text-shadow: 1px 1px 2px #e94560; }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 20px;
      width: 90%;
      max-width: 600px;
      margin-bottom: 30px;
    }
    button {
      border: none;
      border-radius: 16px;
      padding: 30px 20px;
      font-size: 1.2rem;
      font-weight: 700;
      cursor: pointer;
      transition: all 0.3s ease;
      color: white;
      box-shadow: 0 10px 20px rgba(0,0,0,0.3);
      user-select: none;
      -webkit-tap-highlight-color: transparent;
    }
    button:active { transform: translateY(4px); box-shadow: 0 5px 10px rgba(0,0,0,0.3); }
    
    /* Sick Modern Colors */
    .active { background: linear-gradient(135deg, #e94560 0%, #d82b4a 100%); transform: scale(1.02); }
    .inactive { background: #16213e; color: #5a647d; box-shadow: inset 0 2px 5px rgba(0,0,0,0.5); }
    
    .btn-small {
      padding: 15px 30px;
      font-size: 1rem;
      border-radius: 12px;
      width: auto;
      min-width: 200px;
    }
  </style>
</head>
<body>
  <h2>System Control</h2>
  <div class="grid">
    <button id="btn-0" class="inactive" onclick="sendCommand(0)">Levante I</button>
    <button id="btn-1" class="inactive" onclick="sendCommand(1)">Levante D</button>
    <button id="btn-2" class="inactive" onclick="sendCommand(2)">Apertura I</button>
    <button id="btn-3" class="active" onclick="sendCommand(3)">Apertura D</button>
  </div>
  <button id="btn-4" class="btn-small inactive" onclick="sendCommand(4)">Backup (Relay 4)</button>

  <script>
    var gateway = `ws://${window.location.hostname}/ws`;
    var websocket;

    window.addEventListener('load', onLoad);

    function initWebSocket() {
      console.log('Trying to open a WebSocket connection...');
      websocket = new WebSocket(gateway);
      websocket.onopen    = onOpen;
      websocket.onclose   = onClose;
      websocket.onmessage = onMessage;
    }

    function onOpen(event) { console.log('Connection opened'); }
    function onClose(event) { console.log('Connection closed'); setTimeout(initWebSocket, 2000); }
    
    function onMessage(event) {
      updateUI(parseInt(event.data));
    }

    function onLoad(event) { initWebSocket(); }

    function sendCommand(cmd) {
      websocket.send(cmd.toString());
    }

    function updateUI(activeId) {
      for (let i = 0; i <= 4; i++) {
        let btn = document.getElementById('btn-' + i);
        if (i === activeId) {
          btn.classList.remove('inactive');
          btn.classList.add('active');
        } else {
          btn.classList.remove('active');
          btn.classList.add('inactive');
        }
      }
    }
  </script>
</body>
</html>
)rawliteral";

// --- State Update Logic ---
void updateRelays(uint8_t state) {
  currentState = state;
  
  // Reset the inactivity timer every time a valid command is executed
  lastInteractionTime = millis();
  
  // Default to OFF
  digitalWrite(RELAY_1, LOW);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, LOW);
  digitalWrite(RELAY_4, LOW);

  switch(state) {
    case 0: // Levante I
      digitalWrite(RELAY_1, HIGH);
      break;
    case 1: // Levante D
      digitalWrite(RELAY_2, HIGH);
      break;
    case 2: // Apertura I
      digitalWrite(RELAY_3, HIGH);
      digitalWrite(RELAY_4, HIGH);
      break;
    case 3: // Apertura D (All OFF)
      // Already handled by default OFF
      break;
    case 4: // Backup
      digitalWrite(RELAY_4, HIGH);
      break;
  }
  
  // Broadcast new state to all connected web clients
  ws.textAll(String(currentState));
  Serial.printf("State changed to: %d\n", currentState);
}

// --- WebSocket Handler ---
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    // Send current state to new client
    client->text(String(currentState));
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      data[len] = 0;
      uint8_t cmd = atoi((char*)data);
      if(cmd >= 0 && cmd <= 4) {
        updateRelays(cmd);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize Relay Pins
  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT);
  pinMode(RELAY_4, OUTPUT);
  updateRelays(3); // Start with everything OFF (This also initializes the interaction timer)

  // --- Network Setup ---
  WiFi.mode(WIFI_STA);
  
  // Force Static IP
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("STA Failed to configure");
  }

  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid1, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
  }


  // Fallback to Access Point if both fail
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi Failed. Starting Access Point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(apSSID);
    Serial.print("AP IP Address: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("\nWiFi Connected.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    WiFi.setAutoReconnect(true);

  }


  // --- Setup Server ---
  ws.onEvent(onEvent);
  server.addHandler(&ws);
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });
  
  server.begin();
  
  // --- Setup UDP ---
  udp.begin(udpPort);
  Serial.printf("Listening for UDP on port %d\n", udpPort);

}

void loop() {
  ws.cleanupClients(); // Keep WebSockets tidy

  // --- Inactivity Timeout Logic ---
  // If we are not currently in the "All OFF" state AND 60 seconds have passed since the last interaction
  if (currentState != 3 && (millis() - lastInteractionTime >= INACTIVITY_TIMEOUT)) {
    Serial.println("Inactivity timeout reached! Turning all relays OFF.");
    updateRelays(3); 
  }

  // --- UDP Listener Logic ---
  int packetSize = udp.parsePacket();
  if (packetSize) {
    uint8_t buffer[10];
    int len = udp.read(buffer, 10);
    
    // Check packet size and headers (58 58)
    if (len >= 4 && buffer[0] == 0x3A && buffer[1] == 0x3A) { // Note: 58 in decimal is 0x3A in HEX
      uint8_t cmd = buffer[2];
      uint8_t crc = buffer[3];
      
      // Basic CRC validation example: (byte0 + byte1 + cmd) mapped to 8 bits
      uint8_t calculatedCRC = (buffer[0] + buffer[1] + cmd) & 0xFF;
      
      // If you are relying strictly on the headers for now:
      if(cmd >= 0 && cmd <= 4) {
        updateRelays(cmd);
      }
    }
  }
}