-- Track one email notification per low-stock episode.
-- The dashboard resets this timestamp only after stock rises above the 10% threshold.
alter table public.medications
  add column if not exists refill_email_sent_at timestamptz;
