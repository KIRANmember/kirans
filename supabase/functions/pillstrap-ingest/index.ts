import { createClient } from "npm:@supabase/supabase-js@2";

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });

function constantTimeEquals(actual: string, expected: string) {
  const a = new TextEncoder().encode(actual);
  const b = new TextEncoder().encode(expected);
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

async function sha256Hex(value: string) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(digest)].map((v) => v.toString(16).padStart(2, "0")).join("");
}

type Payload = {
  device_code?: string;
  event_id?: string;
  event_type?: string;
  occurred_at?: string;
  firmware_version?: string;
  battery_percent?: number;
  details?: Record<string, unknown>;
};

const allowed = new Set([
  "heartbeat", "dose_alert_received", "reminder_acknowledged",
  "battery_update", "connection_lost", "connection_restored",
  "device_restarted", "test_alert",
]);

Deno.serve(async (request) => {
  if (request.method !== "POST") return json({ error: "Method not allowed" }, 405);

  const deviceSecret = request.headers.get("x-pillstrap-secret") ?? "";
  if (deviceSecret.length < 32 || deviceSecret.length > 256) {
    return json({ error: "Invalid device credentials" }, 401);
  }

  const payload = await request.json().catch(() => null) as Payload | null;
  if (!payload || typeof payload !== "object") return json({ error: "Invalid JSON" }, 400);

  const deviceCode = payload.device_code?.trim();
  const eventId = payload.event_id?.trim();
  const eventType = payload.event_type;
  const occurredAt = payload.occurred_at ? new Date(payload.occurred_at) : new Date();

  if (!deviceCode || deviceCode.length > 64 || !/^[A-Za-z0-9_-]+$/.test(deviceCode)) {
    return json({ error: "Invalid device_code" }, 400);
  }
  if (!eventId || eventId.length > 100 || !/^[A-Za-z0-9_-]+$/.test(eventId)) {
    return json({ error: "Invalid event_id" }, 400);
  }
  if (!eventType || !allowed.has(eventType)) return json({ error: "Invalid event_type" }, 400);
  if (Number.isNaN(occurredAt.getTime())) return json({ error: "Invalid occurred_at" }, 400);
  if (payload.battery_percent !== undefined &&
      (!Number.isInteger(payload.battery_percent) || payload.battery_percent < 0 || payload.battery_percent > 100)) {
    return json({ error: "Invalid battery_percent" }, 400);
  }

  const url = Deno.env.get("SUPABASE_URL");
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!url || !serviceKey) return json({ error: "Server is not configured" }, 503);

  const admin = createClient(url, serviceKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });

  const { data: device, error: lookupError } = await admin
    .from("wearable_devices")
    .select("id,user_id,device_secret_hash,is_active")
    .eq("device_code", deviceCode)
    .maybeSingle();

  if (lookupError) {
    console.error("Device lookup failed", lookupError);
    return json({ error: "Unable to authenticate device" }, 500);
  }
  if (!device || !device.is_active) return json({ error: "Unknown or inactive device" }, 401);

  const receivedHash = await sha256Hex(deviceSecret);
  if (!constantTimeEquals(receivedHash, device.device_secret_hash)) {
    return json({ error: "Invalid device credentials" }, 401);
  }

  const safeDetails = payload.details && typeof payload.details === "object"
    ? payload.details
    : {};

  const { error: eventError } = await admin.from("wearable_events").insert({
    device_id: device.id,
    user_id: device.user_id,
    event_id: eventId,
    event_type: eventType,
    details: safeDetails,
    occurred_at: occurredAt.toISOString(),
  });

  if (eventError && eventError.code !== "23505") {
    console.error("Wearable event insert failed", eventError);
    return json({ error: "Unable to save device event" }, 500);
  }

  const update: Record<string, unknown> = {
    last_seen_at: new Date().toISOString(),
    connection_status: "online",
    updated_at: new Date().toISOString(),
  };
  if (payload.battery_percent !== undefined) update.last_battery_percent = payload.battery_percent;
  if (payload.firmware_version && /^[A-Za-z0-9._+-]{1,32}$/.test(payload.firmware_version)) {
    update.firmware_version = payload.firmware_version;
  }

  const { error: updateError } = await admin
    .from("wearable_devices")
    .update(update)
    .eq("id", device.id);
  if (updateError) {
    console.error("Wearable status update failed", updateError);
    return json({ error: "Event stored but status update failed" }, 500);
  }

  return json({ accepted: true, duplicate: Boolean(eventError), event_id: eventId });
});
