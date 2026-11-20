import { createClient } from "npm:@supabase/supabase-js@2";

const corsHeaders = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { ...corsHeaders, "Content-Type": "application/json; charset=utf-8" },
  });

const escapeHtml = (value: string) => {
  const entities: Record<string, string> = {
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  };
  return value.replace(/[&<>"']/g, (character) => entities[character] ?? character);
};

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response("ok", { headers: corsHeaders });
  if (request.method !== "POST") return json({ error: "Method not allowed" }, 405);

  const authorization = request.headers.get("Authorization") ?? "";
  const token = authorization.startsWith("Bearer ") ? authorization.slice(7) : "";
  if (!token) return json({ error: "Authentication required" }, 401);

  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const serviceRoleKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  const resendApiKey = Deno.env.get("RESEND_API_KEY");
  const fromEmail = Deno.env.get("RESEND_FROM_EMAIL");
  if (!supabaseUrl || !serviceRoleKey || !resendApiKey || !fromEmail) {
    console.error("Refill email function is missing required environment variables.");
    return json({ error: "Refill email service is not configured" }, 503);
  }

  const admin = createClient(supabaseUrl, serviceRoleKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });
  const { data: authData, error: authError } = await admin.auth.getUser(token);
  const user = authData.user;
  if (authError || !user) return json({ error: "Invalid session" }, 401);
  if (!user.email) return json({ error: "No registered email address is available" }, 400);

  const body = await request.json().catch(() => null) as { medication_id?: string } | null;
  const medicationId = body?.medication_id;
  if (!medicationId || !/^[0-9a-f-]{36}$/i.test(medicationId)) {
    return json({ error: "A valid medication_id is required" }, 400);
  }

  const { data: medication, error: medicationError } = await admin
    .from("medications")
    .select("id, user_id, medicine_name, total_pills, pills_remaining, refill_email_sent_at")
    .eq("id", medicationId)
    .eq("user_id", user.id)
    .maybeSingle();
  if (medicationError) {
    console.error("Unable to read medication for refill email", medicationError);
    return json({ error: "Unable to read medication" }, 500);
  }
  if (!medication) return json({ error: "Medication not found" }, 404);

  const threshold = Math.ceil(Number(medication.total_pills) * 0.1);
  if (Number(medication.pills_remaining) > threshold) {
    return json({ sent: false, reason: "threshold_not_reached" });
  }
  if (medication.refill_email_sent_at) {
    return json({ sent: false, reason: "already_notified" });
  }

  // Claim the notification atomically so concurrent dashboard requests cannot send duplicates.
  const claimedAt = new Date().toISOString();
  const { data: claim, error: claimError } = await admin
    .from("medications")
    .update({ refill_email_sent_at: claimedAt })
    .eq("id", medication.id)
    .eq("user_id", user.id)
    .is("refill_email_sent_at", null)
    .select("id")
    .maybeSingle();
  if (claimError) {
    console.error("Unable to claim refill email notification", claimError);
    return json({ error: "Unable to claim refill notification" }, 500);
  }
  if (!claim) return json({ sent: false, reason: "already_notified" });

  const siteUrl = (Deno.env.get("PILLMATE_SITE_URL") ?? "https://www.thekiran.in").replace(/\/$/, "");
  const medicineName = String(medication.medicine_name);
  const remaining = Number(medication.pills_remaining);
  const safeName = escapeHtml(medicineName);
  const subject = `PillMate refill reminder: ${medicineName}`;
  const text = [
    "Your medication has reached its PillMate refill threshold.",
    "",
    `Medicine: ${medicineName}`,
    `Pills remaining: ${remaining}`,
    `Refill threshold: ${threshold} pills (10% of pack size)`,
    "",
    `Review your medications: ${siteUrl}/dashboard/`,
    "",
    "PillMate — A gentler way to stay on time",
  ].join("\n");
  const html = `<div style="font-family:Arial,sans-serif;color:#202334;line-height:1.6;max-width:560px;margin:auto">
    <div style="background:#161a2b;color:#f3f4f6;padding:24px;border-radius:14px">
      <p style="color:#bfa15f;font-size:12px;letter-spacing:2px;text-transform:uppercase;margin:0 0 8px">PillMate refill reminder</p>
      <h1 style="font-size:24px;margin:0 0 12px">Time to plan a refill</h1>
      <p style="color:#d1d5db;margin:0">Your medication has reached its 10% stock threshold.</p>
    </div>
    <div style="padding:22px 4px">
      <h2 style="font-size:20px;margin:0 0 12px">${safeName}</h2>
      <p style="margin:6px 0">Pills remaining: <strong>${remaining}</strong></p>
      <p style="margin:6px 0">Refill threshold: <strong>${threshold} pills</strong></p>
      <p style="margin:18px 0"><a href="${siteUrl}/dashboard/" style="display:inline-block;background:#bfa15f;color:#171717;text-decoration:none;font-weight:bold;padding:12px 18px;border-radius:8px">Open PillMate dashboard</a></p>
      <p style="font-size:12px;color:#6b7280">This is a stock reminder, not confirmation that a dose was taken.</p>
      <p style="font-size:12px;color:#6b7280">PillMate — A gentler way to stay on time</p>
    </div>
  </div>`;

  let providerResponse: Response;
  let providerBody = "";
  try {
    providerResponse = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: { Authorization: `Bearer ${resendApiKey}`, "Content-Type": "application/json" },
      body: JSON.stringify({ from: fromEmail, to: [user.email], subject, text, html }),
    });
    providerBody = await providerResponse.text();
  } catch (error) {
    await admin.from("medications").update({ refill_email_sent_at: null }).eq("id", medication.id).eq("user_id", user.id);
    console.error("Resend request failed", error);
    return json({ error: "Email provider request failed" }, 502);
  }

  if (!providerResponse.ok) {
    await admin.from("medications").update({ refill_email_sent_at: null }).eq("id", medication.id).eq("user_id", user.id);
    console.error("Resend rejected refill email", providerResponse.status, providerBody.slice(0, 500));
    return json({ error: "Email provider rejected the message" }, 502);
  }

  return json({ sent: true, to: user.email, medication_id: medication.id });
});
