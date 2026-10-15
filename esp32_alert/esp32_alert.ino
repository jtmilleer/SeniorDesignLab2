// ESP32 alert bridge for the Lab 2 electric eye.
// The Uno does the filtering and prints "EVT BREACH <level>" / "EVT CLEAR <level>" when the
// beam state changes. This sketch reads those lines and POSTs them to the beam-alert
// Supabase Edge Function, which logs the event and emails the group through Resend.
//
// Wiring: Uno TX (pin 1, 5V) -> 1k -> ESP32 GPIO16, GPIO16 -> 2k -> GND (divides 5V to 3.3V).
//         Uno GND -> ESP32 GND. Do NOT connect anything to Uno RX (pin 0); it breaks uploads.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"   // copy secrets.example.h to secrets.h and fill it in

const int UNO_RX_PIN = 16;
const int UNO_TX_PIN = 17;            // not wired; Serial2 just needs a pin assigned
const long UNO_BAUD = 1200;           // must match Serial.begin() in the Uno sketch
const float THRESHOLD = 0.2;          // keep in sync with threshold_val in the Uno sketch

const unsigned long BREACH_COOLDOWN_MS = 30000;  // minimum time between breach emails
const unsigned long WIFI_WAIT_MS = 10000;
const int SEND_ATTEMPTS = 3;

bool beamBroken = false;
bool breachAlerted = false;      // the current breach has been emailed; a CLEAR is only sent if so
float breachLevel = 0;
bool anyBreachAttempted = false;
unsigned long lastBreachAttemptMs = 0;
String line;

bool waitForWifi()
{
   unsigned long start = millis();
   while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_WAIT_MS)
      delay(250);
   return WiFi.status() == WL_CONNECTED;
}

bool postEvent(const char *event, float level)
{
   if (!waitForWifi())
   {
      Serial.println("WiFi not connected");
      return false;
   }

   WiFiClientSecure client;
   client.setInsecure();   // no certificate pinning; the device token still authenticates us

   HTTPClient http;
   http.begin(client, FUNCTION_URL);
   http.addHeader("Content-Type", "application/json");
   http.addHeader("x-device-token", DEVICE_TOKEN);

   String body = String("{\"event\":\"") + event + "\",\"level\":" + String(level, 3) +
                 ",\"threshold\":" + String(THRESHOLD, 3) +
                 ",\"device\":\"" + DEVICE_NAME + "\"}";
   int code = http.POST(body);
   Serial.printf("POST %s -> %d %s\n", event, code, http.getString().c_str());
   http.end();
   return code >= 200 && code < 300;
}

bool sendWithRetry(const char *event, float level)
{
   for (int attempt = 1; attempt <= SEND_ATTEMPTS; attempt++)
   {
      if (postEvent(event, level))
         return true;
      delay(1000 * attempt);
   }
   Serial.printf("Giving up on %s after %d attempts\n", event, SEND_ATTEMPTS);
   return false;
}

void handleLine(String msg)
{
   msg.trim();
   if (!msg.startsWith("EVT "))
      return;   // ignore the Uno's once-per-second level prints and other debug output

   int space = msg.indexOf(' ', 4);
   String event = msg.substring(4, space < 0 ? msg.length() : space);
   float level = space < 0 ? 0 : msg.substring(space + 1).toFloat();

   if (event == "BREACH")
   {
      beamBroken = true;
      breachAlerted = false;
      breachLevel = level;   // the alert itself goes out from loop() once the cooldown allows
   }
   else if (event == "CLEAR")
   {
      beamBroken = false;
      if (breachAlerted)
         sendWithRetry("CLEAR", level);
      breachAlerted = false;
   }
}

// A breach that arrives during the cooldown (or whose send failed) is alerted as soon as the
// cooldown ends, provided the beam is still broken; one that clears first is dropped.
void alertPendingBreach()
{
   if (!beamBroken || breachAlerted)
      return;
   if (anyBreachAttempted && millis() - lastBreachAttemptMs < BREACH_COOLDOWN_MS)
      return;
   breachAlerted = sendWithRetry("BREACH", breachLevel);
   anyBreachAttempted = true;
   lastBreachAttemptMs = millis();
}

void setup()
{
   Serial.begin(115200);   // USB debug output
   Serial2.setRxBufferSize(1024);   // Uno keeps talking while an HTTPS request blocks
   Serial2.begin(UNO_BAUD, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);

   WiFi.mode(WIFI_STA);
   WiFi.setAutoReconnect(true);
   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
   Serial.print("Connecting to WiFi");
   while (WiFi.status() != WL_CONNECTED)
   {
      delay(500);
      Serial.print(".");
   }
   Serial.printf("\nConnected, IP %s\n", WiFi.localIP().toString().c_str());
}

void loop()
{
   while (Serial2.available())
   {
      char c = Serial2.read();
      if (c == '\n')
      {
         handleLine(line);
         line = "";
      }
      else if (line.length() < 64)
      {
         line += c;
      }
   }
   alertPendingBreach();
}
