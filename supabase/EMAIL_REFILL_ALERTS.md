# PillMate email refill alerts

The dashboard sends one email to the authenticated user's registered Supabase Auth email when a medication is at or below 10% of its original pack size (rounded up).

## Setup

1. Run `supabase/migrations/20261120_email_refill_alerts.sql` in the Supabase SQL Editor.
2. Create a Resend account and verify a sending domain you control.
3. In Supabase Dashboard → Edge Functions → Secrets, add:
   - `RESEND_API_KEY`: the Resend API key
   - `RESEND_FROM_EMAIL`: a sender on the verified domain, e.g. `PillMate <alerts@thekiran.in>`
   - `PILLMATE_SITE_URL`: `https://www.thekiran.in` (optional; this is the default)
4. Deploy the Edge Function:
   `supabase functions deploy send-refill-email`

The Supabase-provided `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY` are used server-side by the Edge Function. Do not put either key, or the Resend key, in dashboard JavaScript.

## Notification behavior

- Existing medications already at or below threshold are checked when the dashboard loads.
- New medications and device-synced medications at/below threshold are checked.
- A stock reduction sends an alert when it crosses from above the threshold to at/below it.
- Only one email is sent during a low-stock episode. Recording a refill that raises stock above threshold resets the notification marker for the next episode.
- The email goes to the user's registered Supabase Auth email, not the caregiver WhatsApp/voice number.
