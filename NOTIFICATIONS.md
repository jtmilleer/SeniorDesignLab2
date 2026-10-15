# Beam-break email notifications

```
Uno (filter, LED) --serial 1200 baud--> ESP32 (WiFi) --HTTPS--> Supabase Edge Function --> Resend --> inbox
                                                                         \--> beam_events table (log)
```

- **Uno** (`lab2_filter_sketch/`): unchanged filtering; on a beam state change it prints
  `EVT BREACH <level>` or `EVT CLEAR <level>`.
- **ESP32** (`esp32_alert/`): reads those lines, waits out a 30 s cooldown between breach emails,
  retries failed sends, and POSTs `{event, level, threshold, device}` to the function.
- **Edge Function** (`supabase/functions/beam-alert/`): checks the `x-device-token` header,
  emails through Resend, and logs every event to `public.beam_events`.

## Wiring

| Uno | ESP32 |
|---|---|
| TX (pin 1) → 1 kΩ → | GPIO16, with 2 kΩ from GPIO16 to GND (5 V → 3.3 V) |
| GND | GND |

Never connect the Uno TX straight to the ESP32: 5 V can damage it. Leave Uno RX (pin 0) unconnected,
or uploads to the Uno will fail. Power the ESP32 from its own USB or from Uno 5V → ESP32 VIN.

## Supabase setup (one time)

```bash
npx supabase login
npx supabase link --project-ref <project-ref>
npx supabase db push                                   # creates beam_events
TOKEN=$(openssl rand -hex 24); echo "$TOKEN"           # copy this into esp32_alert/secrets.h
npx supabase secrets set \
  DEVICE_TOKEN="$TOKEN" \
  RESEND_API_KEY=re_xxx \
  ALERT_TO=you@uiowa.edu,teammate@uiowa.edu \
  GROUP_NAME="Group N"
  # optional: ALERT_FROM="Electric Eye <alerts@your-verified-domain>"
npx supabase functions deploy beam-alert
```

**Resend:** with the default sender `onboarding@resend.dev`, Resend only delivers to the email
address that owns the Resend account. To email the whole group, verify a domain in Resend and set
`ALERT_FROM` to an address on it.

## Test the function without hardware

```bash
curl -i https://<project-ref>.supabase.co/functions/v1/beam-alert \
  -H "x-device-token: <DEVICE_TOKEN>" -H "Content-Type: application/json" \
  -d '{"event":"BREACH","level":0.041,"threshold":0.2,"device":"curl-test"}'
```

Expect `200 {"ok":true}` and an email. A wrong token gives `401`. Function logs are under
Edge Functions → beam-alert → Logs in the Supabase dashboard; events are in the `beam_events` table.

## ESP32 setup

1. Arduino IDE: install the "esp32 by Espressif Systems" board package, select your ESP32 board.
2. Copy `esp32_alert/secrets.example.h` to `esp32_alert/secrets.h` and fill it in
   (2.4 GHz WiFi, e.g. a phone hotspot; eduroam will not work).
3. Upload, open Serial Monitor at 115200. You should see `Connected, IP ...`, then
   `POST BREACH -> 200 ...` when the beam is broken.

The ESP32's `THRESHOLD` constant is only reported in the email; keep it equal to `threshold_val`
in the Uno sketch.
