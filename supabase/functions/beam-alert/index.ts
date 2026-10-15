// beam-alert: called by the ESP32 when the Uno reports a beam state change.
// Emails the group through Resend, then logs the event to public.beam_events.
//
// Secrets (supabase secrets set ...): DEVICE_TOKEN, RESEND_API_KEY, ALERT_TO (comma separated),
// optional ALERT_FROM and GROUP_NAME. SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY are provided.

import { createClient } from "npm:@supabase/supabase-js@2";

const DEVICE_TOKEN = Deno.env.get("DEVICE_TOKEN") ?? "";
const RESEND_API_KEY = Deno.env.get("RESEND_API_KEY") ?? "";
const ALERT_FROM = Deno.env.get("ALERT_FROM") ?? "Electric Eye <onboarding@resend.dev>";
const ALERT_TO = (Deno.env.get("ALERT_TO") ?? "")
  .split(",")
  .map((s) => s.trim())
  .filter(Boolean);
const GROUP_NAME = Deno.env.get("GROUP_NAME") ?? "Group";
const TIME_ZONE = "America/Chicago";

const supabase = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
);

type BeamEvent = {
  event: "BREACH" | "CLEAR";
  level: number;
  threshold: number;
  device: string;
};

function json(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function parseEvent(body: unknown): BeamEvent | null {
  if (typeof body !== "object" || body === null) return null;
  const b = body as Record<string, unknown>;
  if (b.event !== "BREACH" && b.event !== "CLEAR") return null;
  if (typeof b.level !== "number" || typeof b.threshold !== "number") return null;
  if (typeof b.device !== "string" || b.device.length === 0 || b.device.length > 64) return null;
  return { event: b.event, level: b.level, threshold: b.threshold, device: b.device };
}

function buildEmail(evt: BeamEvent, at: Date): { subject: string; text: string } {
  const breach = evt.event === "BREACH";
  const time = at.toLocaleString("en-US", {
    timeZone: TIME_ZONE,
    dateStyle: "medium",
    timeStyle: "long",
  });
  const subject = breach
    ? `[ECE4880 Lab 2 – ${GROUP_NAME}] BEAM BREACH DETECTED`
    : `[ECE4880 Lab 2 – ${GROUP_NAME}] Beam restored`;
  const text = [
    breach ? "Electric Eye alert: the beam was interrupted." : "Electric Eye: the beam has been restored.",
    "",
    `Time:          ${time}`,
    `Device:        ${evt.device}`,
    `Signal level:  ${evt.level.toFixed(3)} (threshold ${evt.threshold.toFixed(3)})`,
    `Status:        ${breach ? "BREACHED" : "CLEAR"}`,
  ].join("\n");
  return { subject, text };
}

// Returns null on success, or an error message.
async function sendEmail(evt: BeamEvent, at: Date): Promise<string | null> {
  const { subject, text } = buildEmail(evt, at);
  const res = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${RESEND_API_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({ from: ALERT_FROM, to: ALERT_TO, subject, text }),
  });
  if (res.ok) return null;
  return `Resend ${res.status}: ${await res.text()}`;
}

Deno.serve(async (req) => {
  if (req.method !== "POST") return json({ error: "method not allowed" }, 405);
  if (!DEVICE_TOKEN || req.headers.get("x-device-token") !== DEVICE_TOKEN) {
    return json({ error: "unauthorized" }, 401);
  }
  if (!RESEND_API_KEY || ALERT_TO.length === 0) {
    return json({ error: "RESEND_API_KEY / ALERT_TO not configured" }, 500);
  }

  let body: unknown;
  try {
    body = await req.json();
  } catch {
    return json({ error: "invalid JSON" }, 400);
  }
  const evt = parseEvent(body);
  if (!evt) return json({ error: "expected {event, level, threshold, device}" }, 400);

  const now = new Date();
  const emailError = await sendEmail(evt, now);
  if (emailError) console.error(emailError);

  // Logging is best effort: a failed insert should not make the ESP32 resend the email.
  const { error: dbError } = await supabase.from("beam_events").insert({
    created_at: now.toISOString(),
    ...evt,
    emailed: emailError === null,
  });
  if (dbError) console.error("beam_events insert failed:", dbError.message);

  // ESP32 retries on non-2xx and prints the body, so pass Resend's reason along.
  if (emailError) return json({ error: "email failed", detail: emailError }, 502);
  return json({ ok: true });
});
