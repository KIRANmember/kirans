import { createClient } from "npm:@supabase/supabase-js@2";

type ExotelEvent = {
  event_name?: string;
  data?: { leg_sid?: string };
  event_data?: { leg_sid?: string };
  leg_sid?: string;
  [key: string]: unknown;
};

type EventResult = {
  alert_event_id: string;
  user_id: string;
  provider_call_id: string;
  medicine_name: string;
  remaining_pills: number;
  next_action: "none" | "speak" | "gather" | "hangup";
};

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });

function constantTimeEquals(actual: string | null, expected: string) {
  if (!actual) return false;

  const encoder = new TextEncoder();
  const left = encoder.encode(actual);
  const right = encoder.encode(expected);
  if (left.length !== right.length) return false;

  let difference = 0;
  for (let index = 0; index < left.length; index += 1) {
    difference |= left[index] ^ right[index];
  }
  return difference === 0;
}

function escapeXml(value: string) {
  return value.replace(/[<>&'"]/g, (character) => ({
    "<": "&lt;",
    ">": "&gt;",
    "&": "&amp;",
    "'": "&apos;",
    '"': "&quot;",
  })[character] ?? character);
}

function cleanSpeech(value: string) {
  return value
    .replace(/[\u0000-\u001F\u007F]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 120);
}

function refillMessage(medicineName: string, remainingPills: number) {
  const medicine = cleanSpeech(medicineName) || "this medicine";
  const pillNoun = remainingPills === 1 ? "pill" : "pills";
  return [
    "Hello. This is PillMate.",
    \`\${medicine} has \${remainingPills} \${pillNoun} remaining.\`,
    "Please arrange a refill soon to avoid an interruption in medication.",
    "Press 1 to acknowledge this alert. Press 9 to stop voice refill alerts.",
  ].join(" ");
}

async function sendExomlAction(
  legId: string,
  exoml: string,
  accountSid: string,
  apiKey: string,
  apiToken: string,
) {
  const response = await fetch(
    \`https://cpaas-api.in.exotel.com/v2/accounts/\${encodeURIComponent(accountSid)}/legs/\${encodeURIComponent(legId)}/actions\`,
    {
      method: "POST",
      headers: {
        Authorization: \`Basic \${btoa(\`\${apiKey}:\${apiToken}\`)}\`,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ exoml }),
    },
  );

  if (response.ok) return;

  const body = (await response.text()).replace(/\s+/g, " ").slice(0, 400);
  throw new Error(
    \`Exotel action failed with HTTP \${response.status}\${body ? \`: \${body}\` : ""}\`,
  );
}

Deno.serve(async (request) => {
  if (request.method !== "POST") {
    return json({ error: "Method not allowed" }, 405);
  }

  const callbackUsername = Deno.env.get("EXOTEL_CALLBACK_USERNAME");
  const callbackPassword = Deno.env.get("EXOTEL_CALLBACK_PASSWORD");
  if (!callbackUsername || !callbackPassword) {
    console.error("Exotel callback authentication is not configured.");
    return json({ error: "Callback authentication is not configured" }, 503);
  }

  const expectedAuthorization = \`Basic \${btoa(\`\${callbackUsername}:\${callbackPassword}\`)}\`;
  if (!constantTimeEquals(
    request.headers.get("authorization"),
    expectedAuthorization,
  )) {
    return json({ error: "Unauthorized" }, 401);
  }

  const payload = await request.json().catch(() => null) as ExotelEvent | null;
  const eventName = payload?.event_name?.trim();
  const legId = payload?.data?.leg_sid
    ?? payload?.event_data?.leg_sid
    ?? payload?.leg_sid;

  if (!payload || !eventName || !legId) {
    return json({ error: "Invalid Exotel event payload" }, 400);
  }

  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const serviceRoleKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  const exotelApiKey = Deno.env.get("EXOTEL_API_KEY");
  const exotelApiToken = Deno.env.get("EXOTEL_API_TOKEN");
  const exotelAccountSid = Deno.env.get("EXOTEL_ACCOUNT_SID");

  if (!supabaseUrl || !serviceRoleKey || !exotelApiKey || !exotelApiToken || !exotelAccountSid) {
    console.error("The Exotel callback function is not fully configured.");
    return json({ error: "Function is not configured" }, 503);
  }

  const client = createClient(supabaseUrl, serviceRoleKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });

  const { data, error } = await client.rpc("record_exotel_voice_event", {
    p_provider_call_id: legId,
    p_event_name: eventName,
    p_payload: payload,
  });

  if (error) {
    console.error("Unable to persist Exotel event", { eventName, legId, error });
    return json({ error: "Unable to persist callback" }, 500);
  }

  const result = (data?.[0] ?? null) as EventResult | null;
  if (!result || result.next_action === "none") {
    return json({ received: true });
  }

  let exoml: string;
  if (result.next_action === "speak") {
    exoml = \`<?xml version="1.0" encoding="UTF-8"?><Flow><StartSay language="en" preferredTTSEngine="polly" pollyVoiceId="Aditi">\${escapeXml(refillMessage(result.medicine_name, result.remaining_pills))}</StartSay></Flow>\`;
  } else if (result.next_action === "gather") {
    exoml = \`<?xml version="1.0" encoding="UTF-8"?><Flow><Gather numDigits="1" timeoutInSec="10"></Gather></Flow>\`;
  } else {
    exoml = \`<?xml version="1.0" encoding="UTF-8"?><Flow><Hangup></Hangup></Flow>\`;
  }

  try {
    await sendExomlAction(
      legId,
      exoml,
      exotelAccountSid,
      exotelApiKey,
      exotelApiToken,
    );
  } catch (actionError) {
    const reason = actionError instanceof Error
      ? actionError.message
      : "Exotel action request failed";

    console.error("Unable to issue Exotel action", { eventName, legId, reason });
    const { error: failureError } = await client.rpc(
      "pillmate_mark_exotel_action_failed",
      {
        p_provider_call_id: legId,
        p_failure_reason: reason,
      },
    );
    if (failureError) console.error("Unable to record action failure", failureError);
  }

  return json({ received: true });
});
