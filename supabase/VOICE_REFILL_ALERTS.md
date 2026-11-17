# Voice refill alerts

This directory adds an Exotel-based, consent-controlled voice refill workflow. It is disabled by default and must be tested only with the developer's explicitly-consented phone number before it is enabled for caregivers.

## What it does

1. A medication reaches or crosses its 10% stock threshold.
2. A durable \`refill_alert_events\` row is created.
3. A database trigger wakes \`send-refill-voice-alerts\`.
4. The function atomically claims one alert, checks consent, calling hours, mode, and retry limit, then creates an Exotel ExoML call leg.
5. Exotel sends authenticated lifecycle events to \`exotel-voice-events\`.
6. On answer, PillMate speaks the medication name and remaining quantity. The caregiver can press:
   - \`1\` to acknowledge the alert.
   - \`9\` to opt out of future voice refill alerts.
7. Failed or unanswered calls receive no more than one retry.

A provider request being accepted, a call being answered, a completed call, and a caregiver acknowledgement are stored separately.

## Required Supabase configuration

The deployed functions already receive \`SUPABASE_URL\` and \`SUPABASE_SERVICE_ROLE_KEY\`. Do not create, expose, or commit replacements for those values.

Create these Edge Function secrets:

| Name | Purpose |
| --- | --- |
| \`PILLMATE_FUNCTION_SECRET\` | A long random value used only between Postgres/cron and \`send-refill-voice-alerts\`. |
| \`VOICE_ALERT_MODE\` | Start with \`disabled\`; use \`test\` for developer-only calls; use \`live\` only after approval and compliance checks. |
| \`VOICE_TEST_PHONE\` | The developer's explicitly-consented E.164 test phone, required in \`test\` mode. |
| \`EXOTEL_API_KEY\` | Exotel API key. |
| \`EXOTEL_API_TOKEN\` | Exotel API token. |
| \`EXOTEL_ACCOUNT_SID\` | Exotel account SID. |
| \`EXOTEL_EXOPHONE\` | Exotel ExoPhone used as caller ID. |
| \`EXOTEL_CALLBACK_USERNAME\` | Dedicated Basic-auth username for Exotel's ExoML event endpoint. |
| \`EXOTEL_CALLBACK_PASSWORD\` | Dedicated Basic-auth password for Exotel's ExoML event endpoint. |

Also create these Supabase Vault secrets. They are not Exotel credentials:

| Vault name | Value |
| --- | --- |
| \`pillmate_function_url\` | \`https://<project-ref>.supabase.co\` |
| \`pillmate_function_secret\` | The exact same value as \`PILLMATE_FUNCTION_SECRET\`. |

Example, using placeholders only:

\`\`\`sql
select vault.create_secret('https://<project-ref>.supabase.co', 'pillmate_function_url');
select vault.create_secret('<same-random-value-as-edge-secret>', 'pillmate_function_secret');
\`\`\`

If either Vault secret is absent, stock updates still succeed and the alert remains queued; the migration intentionally never blocks the refill workflow.

## Deployment order

1. Enable Supabase Vault if it is not already enabled. The migration enables \`pg_net\` and \`pg_cron\`.
2. Apply \`supabase/migrations/20261117_voice_refill_alerts.sql\`.
3. Configure the Edge Function secrets above.
4. Deploy the two functions with JWT verification disabled; both handlers perform their own narrower authentication:

   \`\`\`bash
   supabase functions deploy send-refill-voice-alerts --no-verify-jwt
   supabase functions deploy exotel-voice-events --no-verify-jwt
   \`\`\`

5. Ask Exotel to configure Basic authentication for the ExoML \`leg_event_endpoint\` using \`EXOTEL_CALLBACK_USERNAME\` and \`EXOTEL_CALLBACK_PASSWORD\`. Do not rely only on a hidden callback URL.
6. Start the retry worker after Vault configuration:

   \`\`\`sql
   select public.schedule_pillmate_voice_retry_worker();
   \`\`\`

7. Keep \`VOICE_ALERT_MODE=disabled\` until the trial test is ready.

## Test plan

1. Set \`VOICE_ALERT_MODE=test\` and \`VOICE_TEST_PHONE\` to the developer's own E.164 number.
2. In the dashboard, save that same number as the caregiver voice phone and explicitly tick the consent checkbox.
3. Create a test medication above the threshold, then reduce it to the threshold or below.
4. Confirm that exactly one \`refill_alert_events\` row and one call-attempt row are created.
5. Confirm Exotel's request is recorded as \`provider_accepted\`, then observe \`initiated\`, \`answered\`, and terminal status callbacks.
6. Answer the developer test phone, verify the medicine name and quantity, then press \`1\`. Confirm \`acknowledged_at\` is set.
7. Repeat with no answer. Confirm one retry is scheduled and no third attempt is possible.
8. Press \`9\` in a developer-only call. Confirm the profile's opt-out timestamp is set and subsequent events are not called.
9. Verify that an event for any number other than \`VOICE_TEST_PHONE\` is recorded as \`test_mode_skipped\`; it must never call that number.

## Production gate

Before changing to \`VOICE_ALERT_MODE=live\`, obtain the caregiver's explicit consent, confirm the caller ID/ExoPhone and Exotel KYC requirements, configure callback authentication, verify permitted calling hours and DND/TRAI obligations, and document the approved call script. Do not enable call recording by default.
