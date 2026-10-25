// ESP32 alert bridge for the Lab 2 electric eye.
// The Uno does the filtering and drives its ALERT_PIN HIGH while the beam is broken, LOW while
// it is clear. This sketch watches that line and POSTs each state change to the beam-alert
// Supabase Edge Function, which logs the event and emails the group through Resend.
//
// Wiring: Uno pin 8 (5V) -> 1k -> ESP32 GPIO22, GPIO22 -> 2k -> GND (divides 5V to 3.3V).
//         Uno GND -> ESP32 GND. The 2k also holds GPIO22 LOW (clear) if the Uno is unplugged.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "secrets.h"   // copy secrets.example.h to secrets.h and fill it in

const int UNO_ALERT_PIN = 22;         // HIGH = beam broken
const float THRESHOLD = 0.2;          // keep in sync with threshold_val in the Uno sketch (email only)
const int LED_PIN = 2;                // onboard LED on most ESP32 dev boards; mirrors the beam state

const unsigned long DEBOUNCE_MS = 50;            // the pin must hold a new level this long to count
const unsigned long BREACH_COOLDOWN_MS = 30000;  // minimum time between breach emails
const unsigned long WIFI_WAIT_MS = 10000;
const int SEND_ATTEMPTS = 3;

bool beamBroken = false;
bool breachAlerted = false;      // the current breach has been emailed; a CLEAR is only sent if so
bool anyBreachAttempted = false;
unsigned long lastBreachAttemptMs = 0;
int lastReading = LOW;
unsigned long lastChangeMs = 0;

// An HTTPS send blocks loop() for a few seconds, so a breach that starts and ends during one would
// never be polled. The interrupt latches any HIGH pulse of at least DEBOUNCE_MS so it is still alerted.
volatile unsigned long riseMs = 0;
volatile bool shortBreach = false;

void IRAM_ATTR onAlertPinChange()
{
   if (digitalRead(UNO_ALERT_PIN) == HIGH)
      riseMs = millis();
   else if (millis() - riseMs >= DEBOUNCE_MS)
      shortBreach = true;
}

bool waitForWifi()
{
   unsigned long start = millis();
   while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_WAIT_MS)
      delay(250);
   return WiFi.status() == WL_CONNECTED;
}

// The digital line carries no signal level, so level is sent as 0.
bool postEvent(const char *event)
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

   String body = String("{\"event\":\"") + event + "\",\"level\":0" +
                 ",\"threshold\":" + String(THRESHOLD, 3) +
                 ",\"device\":\"" + DEVICE_NAME + "\"}";
   int code = http.POST(body);
   Serial.printf("POST %s -> %d %s\n", event, code, http.getString().c_str());
   http.end();
   return code >= 200 && code < 300;
}

bool sendWithRetry(const char *event)
{
   for (int attempt = 1; attempt <= SEND_ATTEMPTS; attempt++)
   {
      if (postEvent(event))
         return true;
      delay(1000 * attempt);
   }
   Serial.printf("Giving up on %s after %d attempts\n", event, SEND_ATTEMPTS);
   return false;
}

// Reads the Uno's alert line and acts once a new level has been stable for DEBOUNCE_MS.
void readAlertPin()
{
   // A breach that came and went while loop() was blocked: report it now; the CLEAR follows
   // on the next call, once the pin reads LOW again.
   if (shortBreach && !beamBroken)
   {
      shortBreach = false;
      beamBroken = true;
      breachAlerted = false;
      digitalWrite(LED_PIN, HIGH);
      Serial.println(">>> Uno reported a short BREACH");
      return;
   }

   int reading = digitalRead(UNO_ALERT_PIN);
   if (reading != lastReading)
   {
      lastReading = reading;
      lastChangeMs = millis();
      return;
   }
   bool nowBroken = reading == HIGH;
   if (nowBroken == beamBroken || millis() - lastChangeMs < DEBOUNCE_MS)
      return;

   beamBroken = nowBroken;
   digitalWrite(LED_PIN, beamBroken ? HIGH : LOW);
   Serial.printf(">>> Uno reports %s\n", beamBroken ? "BREACH" : "CLEAR");

   if (beamBroken)
   {
      shortBreach = false;     // this breach is being handled here; don't report it twice
      breachAlerted = false;   // the alert itself goes out from loop() once the cooldown allows
   }
   else
   {
      shortBreach = false;     // set by this same breach's falling edge; it was already handled
      if (breachAlerted)
         sendWithRetry("CLEAR");
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
   breachAlerted = sendWithRetry("BREACH");
   anyBreachAttempted = true;
   lastBreachAttemptMs = millis();
}

void setup()
{
   Serial.begin(115200);   // USB debug output
   pinMode(LED_PIN, OUTPUT);
   pinMode(UNO_ALERT_PIN, INPUT);   // external 2k divider resistor pulls it LOW
   attachInterrupt(digitalPinToInterrupt(UNO_ALERT_PIN), onAlertPinChange, CHANGE);

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
   readAlertPin();
   alertPendingBreach();
}
