import { createClient } from "npm:@supabase/supabase-js@2";

type Claim = {
  attempt_id: string;
  alert_event_id: string;
  attempt_number: number;
  caregiver_phone: string;
  medicine_name: string;
  remaining_pills: number;
};

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json; charset=utf-8" },
  });

function constantTimeEquals(actual: string | null, expected: string | undefined) {
  if (!actual || !expected) return false;

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

function providerError(status: number, body: string) {
  const compactBody = body.replace(/\s+/g, " ").slice(0, 400);
  return \`Exotel leg request failed with HTTP \${status}\${compactBody ? \`: \${compactBody}\` : ""}\`;
}

Deno.serve(async (request) => {
  if (request.method !== "POST") {
    return json({ error: "Method not allowed" }, 405);
  }

  if (!constantTimeEquals(
    request.headers.get("x-pillmate-function-secret"),
    Deno.env.get("PILLMATE_FUNCTION_SECRET"),
  )) {
    return json({ error: "Unauthorized" }, 401);
  }

  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const serviceRoleKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!supabaseUrl || !serviceRoleKey) {
    console.error("Supabase Edge Function environment is incomplete.");
    return json({ error: "Function is not configured" }, 500);
  }

  const mode = (Deno.env.get("VOICE_ALERT_MODE") ?? "disabled").toLowerCase();
  if (!["disabled", "test", "live"].includes(mode)) {
    return json({ error: "VOICE_ALERT_MODE must be disabled, test, or live" }, 500);
  }

  const body = await request.json().catch(() => null) as {
    scan?: boolean;
    record?: { id?: string };
  } | null;

  const client = createClient(supabaseUrl, serviceRoleKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });

  let eventIds: string[] = [];
  if (body?.scan) {
    const { data, error } = await client.rpc("list_due_voice_refill_alerts", {
      p_limit: 20,
    });
    if (error) {
      console.error("Unable to list due voice alerts", error);
      return json({ error: "Unable to list due alerts" }, 500);
    }
    eventIds = (data ?? []).map((row: { id: string }) => row.id);
  } else if (body?.record?.id) {
    eventIds = [body.record.id];
  } else {
    return json({ error: "Expected a refill alert event id or scan=true" }, 400);
  }

  if (mode === "disabled") {
    return json({
      accepted: true,
      mode,
      processed: 0,
      detail: "Voice alerts are disabled; queued events were left untouched.",
    });
  }

  const exotelApiKey = Deno.env.get("EXOTEL_API_KEY");
  const exotelApiToken = Deno.env.get("EXOTEL_API_TOKEN");
  const exotelAccountSid = Deno.env.get("EXOTEL_ACCOUNT_SID");
  const exotelExophone = Deno.env.get("EXOTEL_EXOPHONE");
  const testPhone = Deno.env.get("VOICE_TEST_PHONE");

  if (!exotelApiKey || !exotelApiToken || !exotelAccountSid || !exotelExophone) {
    console.error("Exotel credentials or ExoPhone are missing.");
    return json({ error: "Voice provider is not configured" }, 503);
  }

  if (mode === "test" && !testPhone) {
    return json({ error: "VOICE_TEST_PHONE is required in test mode" }, 503);
  }

  const eventEndpoint = \`\${supabaseUrl}/functions/v1/exotel-voice-events\`;
  const authorization = \`Basic \${btoa(\`\${exotelApiKey}:\${exotelApiToken}\`)}\`;
  const results: Array<{ eventId: string; outcome: string }> = [];

  for (const eventId of eventIds) {
    const { data: claimedRows, error: claimError } = await client.rpc(
      "claim_voice_refill_alert",
      { p_event_id: eventId },
    );

    if (claimError) {
      console.error("Unable to claim voice alert", { eventId, claimError });
      results.push({ eventId, outcome: "claim_failed" });
      continue;
    }

    const claim = (claimedRows?.[0] ?? null) as Claim | null;
    if (!claim) {
      results.push({ eventId, outcome: "not_due_or_not_eligible" });
      continue;
    }

    if (mode === "test" && claim.caregiver_phone !== testPhone) {
      const { error } = await client.rpc("pillmate_mark_test_mode_skipped", {
        p_attempt_id: claim.attempt_id,
      });
      if (error) console.error("Unable to mark test-mode skip", error);
      results.push({ eventId, outcome: "skipped_non_test_phone" });
      continue;
    }

    let providerResponse: Response;
    let providerBody = "";
    try {
      providerResponse = await fetch(
        \`https://cpaas-api.in.exotel.com/v2/accounts/\${encodeURIComponent(exotelAccountSid)}/legs\`,
        {
          method: "POST",
          headers: {
            Authorization: authorization,
            "Content-Type": "application/json",
          },
          body: JSON.stringify({
            contact_uri: claim.caregiver_phone,
            exophone: exotelExophone,
            leg_event_endpoint: eventEndpoint,
          }),
        },
      );
      providerBody = await providerResponse.text();
    } catch (error) {
      const reason = error instanceof Error ? error.message : "Network request failed";
      await client.rpc("pillmate_mark_voice_attempt_failed", {
        p_attempt_id: claim.attempt_id,
        p_failure_reason: \`Exotel request error: \${reason}\`,
      });
      results.push({ eventId, outcome: "provider_request_failed" });
      continue;
    }

    if (!providerResponse.ok) {
      await client.rpc("pillmate_mark_voice_attempt_failed", {
        p_attempt_id: claim.attempt_id,
        p_failure_reason: providerError(providerResponse.status, providerBody),
      });
      results.push({ eventId, outcome: "provider_rejected_request" });
      continue;
    }

    let providerJson: { data?: { leg_sid?: string }; leg_sid?: string } = {};
    try {
      providerJson = JSON.parse(providerBody);
    } catch {
      // The missing id is handled below and recorded as a failure.
    }

    const legId = providerJson.data?.leg_sid ?? providerJson.leg_sid;
    if (!legId) {
      await client.rpc("pillmate_mark_voice_attempt_failed", {
        p_attempt_id: claim.attempt_id,
        p_failure_reason: "Exotel accepted the request without returning a leg id.",
      });
      results.push({ eventId, outcome: "provider_response_missing_leg_id" });
      continue;
    }

    const { error: acceptedError } = await client.rpc(
      "pillmate_mark_voice_request_accepted",
      {
        p_attempt_id: claim.attempt_id,
        p_provider_call_id: legId,
      },
    );

    if (acceptedError) {
      console.error("Provider accepted a leg but its id could not be persisted", {
        eventId,
        legId,
        acceptedError,
      });
      results.push({ eventId, outcome: "accepted_but_persistence_failed" });
      continue;
    }

    results.push({ eventId, outcome: "provider_accepted" });
  }

  return json({ accepted: true, mode, results });
});
