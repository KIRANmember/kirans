-- PillMate voice refill alerts.
-- This migration is intentionally opt-in: the Edge Function defaults to disabled until
-- VOICE_ALERT_MODE and all required secrets are configured.

create extension if not exists pg_net with schema extensions;
create extension if not exists pg_cron;

alter table public.profiles
  add column if not exists voice_call_phone text,
  add column if not exists voice_call_consent_at timestamptz,
  add column if not exists voice_call_consent_version text,
  add column if not exists voice_call_opted_out_at timestamptz,
  add column if not exists voice_call_window_start time not null default time '08:00',
  add column if not exists voice_call_window_end time not null default time '20:00';

do $$
begin
  if not exists (
    select 1 from pg_constraint where conname = 'profiles_voice_call_phone_e164_check'
  ) then
    alter table public.profiles
      add constraint profiles_voice_call_phone_e164_check
      check (voice_call_phone is null or voice_call_phone ~ '^\+[1-9][0-9]{7,14}$');
  end if;

  if not exists (
    select 1 from pg_constraint where conname = 'profiles_voice_call_window_check'
  ) then
    alter table public.profiles
      add constraint profiles_voice_call_window_check
      check (voice_call_window_start < voice_call_window_end);
  end if;
end;
$$;

-- The original schema caps stock at one pack, which prevents "Record refill" from
-- adding a newly received pack to pills that are still on hand.
alter table public.medications
  drop constraint if exists medications_pills_remaining_check;

alter table public.medications
  add column if not exists low_stock_cycle integer not null default 0;

do $$
begin
  if not exists (
    select 1 from pg_constraint where conname = 'medications_pills_remaining_nonnegative_check'
  ) then
    alter table public.medications
      add constraint medications_pills_remaining_nonnegative_check
      check (pills_remaining >= 0);
  end if;

  if not exists (
    select 1 from pg_constraint where conname = 'medications_low_stock_cycle_check'
  ) then
    alter table public.medications
      add constraint medications_low_stock_cycle_check
      check (low_stock_cycle >= 0);
  end if;
end;
$$;

do $$
begin
  if to_regclass('public.refill_alert_events') is null then
    execute $table$
      create table public.refill_alert_events (
        id uuid primary key default gen_random_uuid(),
        medication_id uuid not null references public.medications(id) on delete cascade,
        user_id uuid not null references auth.users(id) on delete cascade,
        low_stock_cycle integer not null,
        medicine_name text not null,
        remaining_pills integer not null check (remaining_pills >= 0),
        voice_status text not null default 'awaiting_consent' check (
          voice_status in (
            'awaiting_consent', 'pending', 'claimed', 'provider_accepted',
            'initiated', 'ringing', 'answered', 'awaiting_acknowledgement',
            'acknowledged', 'completed', 'no_answer', 'failed',
            'retry_scheduled', 'test_mode_skipped', 'opted_out'
          )
        ),
        voice_provider text,
        provider_call_id text,
        call_request_accepted_at timestamptz,
        call_initiated_at timestamptz,
        call_answered_at timestamptz,
        call_completed_at timestamptz,
        call_duration_seconds integer check (call_duration_seconds is null or call_duration_seconds >= 0),
        failure_reason text,
        acknowledged_at timestamptz,
        call_attempt_count integer not null default 0 check (call_attempt_count >= 0),
        retry_count integer not null default 0 check (retry_count between 0 and 1),
        last_retry_at timestamptz,
        next_retry_at timestamptz,
        claimed_at timestamptz,
        claim_lease_expires_at timestamptz,
        created_at timestamptz not null default now(),
        updated_at timestamptz not null default now(),
        unique (medication_id, low_stock_cycle)
      )
    $table$;

    perform set_config('pillmate_voice.created_refill_alert_events', 'true', false);
  else
    perform set_config('pillmate_voice.created_refill_alert_events', 'false', false);
  end if;
end;
$$;

alter table public.refill_alert_events
  add column if not exists medication_id uuid references public.medications(id) on delete cascade,
  add column if not exists user_id uuid references auth.users(id) on delete cascade,
  add column if not exists low_stock_cycle integer not null default 0,
  add column if not exists medicine_name text,
  add column if not exists remaining_pills integer,
  add column if not exists voice_status text not null default 'awaiting_consent',
  add column if not exists voice_provider text,
  add column if not exists provider_call_id text,
  add column if not exists call_request_accepted_at timestamptz,
  add column if not exists call_initiated_at timestamptz,
  add column if not exists call_answered_at timestamptz,
  add column if not exists call_completed_at timestamptz,
  add column if not exists call_duration_seconds integer,
  add column if not exists failure_reason text,
  add column if not exists acknowledged_at timestamptz,
  add column if not exists call_attempt_count integer not null default 0,
  add column if not exists retry_count integer not null default 0,
  add column if not exists last_retry_at timestamptz,
  add column if not exists next_retry_at timestamptz,
  add column if not exists claimed_at timestamptz,
  add column if not exists claim_lease_expires_at timestamptz,
  add column if not exists created_at timestamptz not null default now(),
  add column if not exists updated_at timestamptz not null default now();

-- Only a newly-created voice event table has the cycle-based uniqueness
-- contract. The existing WhatsApp alert history may legitimately have more
-- than one row per medication and must remain untouched.
do $voice_unique$
begin
  if current_setting('pillmate_voice.created_refill_alert_events', true) = 'true'
     and not exists (
       select 1 from pg_constraint where conname = 'refill_alert_events_medication_cycle_key'
     ) then
    alter table public.refill_alert_events
      add constraint refill_alert_events_medication_cycle_key
      unique (medication_id, low_stock_cycle);
  end if;
end;
$voice_unique$;

create table if not exists public.refill_alert_call_attempts (
  id uuid primary key default gen_random_uuid(),
  refill_alert_event_id uuid not null references public.refill_alert_events(id) on delete cascade,
  attempt_number integer not null check (attempt_number between 1 and 2),
  provider text not null default 'exotel',
  provider_call_id text unique,
  status text not null default 'claimed',
  request_accepted_at timestamptz,
  initiated_at timestamptz,
  answered_at timestamptz,
  completed_at timestamptz,
  duration_seconds integer check (duration_seconds is null or duration_seconds >= 0),
  failure_reason text,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  unique (refill_alert_event_id, attempt_number)
);

create table if not exists public.refill_alert_provider_events (
  id bigint generated always as identity primary key,
  provider text not null,
  provider_call_id text not null,
  event_name text not null,
  event_fingerprint text not null,
  payload jsonb not null,
  received_at timestamptz not null default now(),
  unique (provider, provider_call_id, event_name, event_fingerprint)
);

create index if not exists refill_alert_events_due_voice_idx
  on public.refill_alert_events (voice_status, next_retry_at)
  where voice_status in ('pending', 'retry_scheduled');

create index if not exists refill_alert_call_attempts_provider_call_idx
  on public.refill_alert_call_attempts (provider, provider_call_id);

alter table public.refill_alert_events enable row level security;
alter table public.refill_alert_call_attempts enable row level security;
alter table public.refill_alert_provider_events enable row level security;

create or replace function public.pillmate_set_updated_at()
returns trigger
language plpgsql
set search_path = public
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

drop trigger if exists pillmate_profiles_set_updated_at on public.profiles;
create trigger pillmate_profiles_set_updated_at
before update on public.profiles
for each row execute function public.pillmate_set_updated_at();

drop trigger if exists pillmate_refill_events_set_updated_at on public.refill_alert_events;
create trigger pillmate_refill_events_set_updated_at
before update on public.refill_alert_events
for each row execute function public.pillmate_set_updated_at();

drop trigger if exists pillmate_refill_attempts_set_updated_at on public.refill_alert_call_attempts;
create trigger pillmate_refill_attempts_set_updated_at
before update on public.refill_alert_call_attempts
for each row execute function public.pillmate_set_updated_at();

create or replace function public.pillmate_prepare_low_stock_cycle()
returns trigger
language plpgsql
set search_path = public
as $$
declare
  old_threshold integer;
  new_threshold integer;
begin
  new_threshold := ceil(new.total_pills * 0.10)::integer;

  if tg_op = 'INSERT' then
    new.low_stock_cycle := coalesce(new.low_stock_cycle, 0);
    if new.active and new.pills_remaining <= new_threshold then
      new.low_stock_cycle := new.low_stock_cycle + 1;
    end if;
    return new;
  end if;

  old_threshold := ceil(old.total_pills * 0.10)::integer;
  new.low_stock_cycle := old.low_stock_cycle;

  if new.active
     and new.pills_remaining <= new_threshold
     and (not old.active or old.pills_remaining > old_threshold) then
    new.low_stock_cycle := old.low_stock_cycle + 1;
  end if;

  return new;
end;
$$;

create or replace function public.pillmate_create_low_stock_refill_event()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
declare
  threshold integer;
  initial_status text;
begin
  threshold := ceil(new.total_pills * 0.10)::integer;

  if not new.active or new.pills_remaining > threshold or new.low_stock_cycle = 0 then
    return new;
  end if;

  select case
    when p.voice_call_phone is not null
      and p.voice_call_consent_at is not null
      and p.voice_call_opted_out_at is null
    then 'pending'
    else 'awaiting_consent'
  end
  into initial_status
  from public.profiles p
  where p.id = new.user_id;

  insert into public.refill_alert_events (
    medication_id,
    user_id,
    low_stock_cycle,
    medicine_name,
    remaining_pills,
    voice_status,
    next_retry_at
  ) values (
    new.id,
    new.user_id,
    new.low_stock_cycle,
    new.medicine_name,
    new.pills_remaining,
    coalesce(initial_status, 'awaiting_consent'),
    now()
  )
  on conflict (medication_id, low_stock_cycle) do nothing;

  return new;
end;
$$;

-- Only install stock-event triggers when this migration created the event table.
-- A project that already has refill_alert_events keeps its existing low-stock logic.
do $$
begin
  if current_setting('pillmate_voice.created_refill_alert_events', true) = 'true' then
    execute 'drop trigger if exists pillmate_prepare_low_stock_cycle on public.medications';
    execute '
      create trigger pillmate_prepare_low_stock_cycle
      before insert or update of pills_remaining, total_pills, active
      on public.medications
      for each row execute function public.pillmate_prepare_low_stock_cycle()
    ';

    execute 'drop trigger if exists pillmate_create_low_stock_refill_event on public.medications';
    execute '
      create trigger pillmate_create_low_stock_refill_event
      after insert or update of pills_remaining, total_pills, active
      on public.medications
      for each row execute function public.pillmate_create_low_stock_refill_event()
    ';
  end if;
end;
$$;

create or replace function public.pillmate_refresh_voice_consent_events()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
  if new.voice_call_phone is not null
     and new.voice_call_consent_at is not null
     and new.voice_call_opted_out_at is null then
    update public.refill_alert_events
    set voice_status = 'pending',
        next_retry_at = now(),
        failure_reason = null,
        claim_lease_expires_at = null
    where user_id = new.id
      and voice_status = 'awaiting_consent';
  elsif new.voice_call_opted_out_at is not null then
    update public.refill_alert_events
    set voice_status = 'opted_out',
        next_retry_at = null,
        claim_lease_expires_at = null
    where user_id = new.id
      and voice_status in (
        'awaiting_consent', 'pending', 'claimed', 'provider_accepted',
        'initiated', 'ringing', 'answered', 'awaiting_acknowledgement',
        'retry_scheduled'
      );
  end if;

  return new;
end;
$$;

drop trigger if exists pillmate_refresh_voice_consent_events on public.profiles;
create trigger pillmate_refresh_voice_consent_events
after insert or update of voice_call_phone, voice_call_consent_at, voice_call_opted_out_at
on public.profiles
for each row execute function public.pillmate_refresh_voice_consent_events();

create or replace function public.pillmate_enqueue_voice_refill_alert()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
declare
  function_url text;
  function_secret text;
begin
  select decrypted_secret into function_url
  from vault.decrypted_secrets
  where name = 'pillmate_function_url'
  limit 1;

  select decrypted_secret into function_secret
  from vault.decrypted_secrets
  where name = 'pillmate_function_secret'
  limit 1;

  if function_url is null or function_secret is null then
    raise warning 'PillMate voice alert is queued, but Vault setup is incomplete.';
    return new;
  end if;

  perform net.http_post(
    url := function_url || '/functions/v1/send-refill-voice-alerts',
    headers := jsonb_build_object(
      'Content-Type', 'application/json',
      'x-pillmate-function-secret', function_secret
    ),
    body := jsonb_build_object(
      'type', 'INSERT',
      'table', 'refill_alert_events',
      'schema', 'public',
      'record', jsonb_build_object('id', new.id)
    )
  );
exception
  when others then
    -- A notification outage must never prevent stock or refill updates.
    raise warning 'PillMate voice alert enqueue failed: %', sqlerrm;
end;
$$;

-- Adapt the established WhatsApp low-stock event stream to voice alerts.
-- This does not change the existing low-stock threshold or WhatsApp trigger.
create or replace function public.pillmate_initialize_voice_refill_alert()
returns trigger
language plpgsql
security definer
set search_path = public
as $voice_initialize$
declare
  initial_status text := 'awaiting_consent';
  medicine_value text;
begin
  if new.event_type <> 'low_stock_detected' then
    return new;
  end if;

  select
    case
      when p.voice_call_phone is not null
       and p.voice_call_consent_at is not null
       and p.voice_call_opted_out_at is null
      then 'pending'
      else 'awaiting_consent'
    end,
    m.medicine_name
  into initial_status, medicine_value
  from public.profiles p
  join public.medications m on m.id = new.medication_id
  where p.id = new.user_id;

  update public.refill_alert_events
  set voice_status = coalesce(initial_status, 'awaiting_consent'),
      medicine_name = coalesce(medicine_name, medicine_value),
      remaining_pills = coalesce(remaining_pills, new.stock_remaining),
      next_retry_at = case
        when initial_status = 'pending' then now()
        else null
      end
  where id = new.id;

  return new;
end;
$voice_initialize$;

-- This name runs before the enqueue trigger alphabetically, ensuring an
-- eligible new WhatsApp event is queued with its voice status already set.
drop trigger if exists aaa_pillmate_initialize_voice_refill_alert on public.refill_alert_events;
create trigger aaa_pillmate_initialize_voice_refill_alert
after insert on public.refill_alert_events
for each row execute function public.pillmate_initialize_voice_refill_alert();

drop trigger if exists pillmate_enqueue_voice_refill_alert on public.refill_alert_events;
create trigger pillmate_enqueue_voice_refill_alert
after insert on public.refill_alert_events
for each row execute function public.pillmate_enqueue_voice_refill_alert();

create or replace function public.claim_voice_refill_alert(p_event_id uuid)
returns table (
  attempt_id uuid,
  alert_event_id uuid,
  attempt_number integer,
  caregiver_phone text,
  medicine_name text,
  remaining_pills integer
)
language plpgsql
security definer
set search_path = public
as $$
declare
  event_row public.refill_alert_events%rowtype;
  phone text;
  attempt_id_value uuid;
  attempt_number_value integer;
begin
  select e.*
  into event_row
  from public.refill_alert_events e
  join public.profiles p on p.id = e.user_id
  where e.id = p_event_id
    and e.voice_status in ('pending', 'retry_scheduled')
    and e.call_attempt_count < 2
    and (e.next_retry_at is null or e.next_retry_at <= now())
    and (e.claim_lease_expires_at is null or e.claim_lease_expires_at < now())
    and p.voice_call_phone is not null
    and p.voice_call_consent_at is not null
    and p.voice_call_opted_out_at is null
    and (now() at time zone 'Asia/Kolkata')::time
      between p.voice_call_window_start and p.voice_call_window_end
  for update of e skip locked;

  if not found then
    return;
  end if;

  select voice_call_phone into phone
  from public.profiles
  where id = event_row.user_id;

  attempt_number_value := event_row.call_attempt_count + 1;

  update public.refill_alert_events
  set voice_status = 'claimed',
      call_attempt_count = attempt_number_value,
      retry_count = greatest(attempt_number_value - 1, 0),
      last_retry_at = case
        when attempt_number_value > 1 then now()
        else last_retry_at
      end,
      claimed_at = now(),
      claim_lease_expires_at = now() + interval '5 minutes',
      next_retry_at = null
  where id = event_row.id
  returning * into event_row;

  insert into public.refill_alert_call_attempts (
    refill_alert_event_id,
    attempt_number,
    provider,
    status
  ) values (
    event_row.id,
    attempt_number_value,
    'exotel',
    'claimed'
  )
  returning id into attempt_id_value;

  return query
  select
    attempt_id_value,
    event_row.id,
    attempt_number_value,
    phone,
    event_row.medicine_name,
    event_row.remaining_pills;
end;
$$;

create or replace function public.pillmate_mark_voice_request_accepted(
  p_attempt_id uuid,
  p_provider_call_id text
)
returns void
language plpgsql
security definer
set search_path = public
as $$
declare
  event_id_value uuid;
begin
  update public.refill_alert_call_attempts
  set provider_call_id = p_provider_call_id,
      status = 'provider_accepted',
      request_accepted_at = now()
  where id = p_attempt_id
  returning refill_alert_event_id into event_id_value;

  if event_id_value is null then
    raise exception 'Unknown refill alert attempt';
  end if;

  update public.refill_alert_events
  set voice_provider = 'exotel',
      provider_call_id = p_provider_call_id,
      voice_status = 'provider_accepted',
      call_request_accepted_at = now(),
      claim_lease_expires_at = now()
  where id = event_id_value;
end;
$$;

create or replace function public.pillmate_mark_voice_attempt_failed(
  p_attempt_id uuid,
  p_failure_reason text
)
returns void
language plpgsql
security definer
set search_path = public
as $$
declare
  event_row public.refill_alert_events%rowtype;
  attempt_row public.refill_alert_call_attempts%rowtype;
begin
  select a, e
  into attempt_row, event_row
  from public.refill_alert_call_attempts a
  join public.refill_alert_events e on e.id = a.refill_alert_event_id
  where a.id = p_attempt_id
  for update of a, e;

  if not found then
    return;
  end if;

  update public.refill_alert_call_attempts
  set status = 'failed',
      failure_reason = left(coalesce(p_failure_reason, 'Provider request failed'), 500),
      completed_at = coalesce(completed_at, now())
  where id = attempt_row.id;

  if event_row.call_attempt_count < 2 then
    update public.refill_alert_events
    set voice_status = 'retry_scheduled',
        failure_reason = left(coalesce(p_failure_reason, 'Provider request failed'), 500),
        next_retry_at = now() + interval '15 minutes',
        claim_lease_expires_at = null
    where id = event_row.id;
  else
    update public.refill_alert_events
    set voice_status = 'failed',
        failure_reason = left(coalesce(p_failure_reason, 'Provider request failed'), 500),
        call_completed_at = coalesce(call_completed_at, now()),
        claim_lease_expires_at = null
    where id = event_row.id;
  end if;
end;
$$;

create or replace function public.pillmate_mark_test_mode_skipped(p_attempt_id uuid)
returns void
language plpgsql
security definer
set search_path = public
as $$
declare
  event_id_value uuid;
begin
  update public.refill_alert_call_attempts
  set status = 'test_mode_skipped',
      completed_at = now(),
      failure_reason = 'Test mode only permits the configured developer phone number.'
  where id = p_attempt_id
  returning refill_alert_event_id into event_id_value;

  update public.refill_alert_events
  set voice_status = 'test_mode_skipped',
      failure_reason = 'Test mode only permits the configured developer phone number.',
      claim_lease_expires_at = null,
      next_retry_at = null
  where id = event_id_value;
end;
$$;

create or replace function public.list_due_voice_refill_alerts(p_limit integer default 20)
returns table (id uuid)
language sql
security definer
set search_path = public
as $$
  select e.id
  from public.refill_alert_events e
  where e.voice_status in ('pending', 'retry_scheduled')
    and (e.next_retry_at is null or e.next_retry_at <= now())
    and (e.claim_lease_expires_at is null or e.claim_lease_expires_at < now())
  order by e.created_at
  limit greatest(1, least(coalesce(p_limit, 20), 50));
$$;

create or replace function public.record_exotel_voice_event(
  p_provider_call_id text,
  p_event_name text,
  p_payload jsonb
)
returns table (
  alert_event_id uuid,
  user_id uuid,
  provider_call_id text,
  medicine_name text,
  remaining_pills integer,
  next_action text
)
language plpgsql
security definer
set search_path = public
as $$
declare
  event_row public.refill_alert_events%rowtype;
  attempt_row public.refill_alert_call_attempts%rowtype;
  event_inserted integer;
  digits text;
  action_value text := 'none';
  terminal_reason text;
begin
  select a, e
  into attempt_row, event_row
  from public.refill_alert_call_attempts a
  join public.refill_alert_events e on e.id = a.refill_alert_event_id
  where a.provider = 'exotel'
    and a.provider_call_id = p_provider_call_id
  for update of a, e;

  if not found then
    return;
  end if;

  insert into public.refill_alert_provider_events (
    provider,
    provider_call_id,
    event_name,
    event_fingerprint,
    payload
  ) values (
    'exotel',
    p_provider_call_id,
    p_event_name,
    md5(p_payload::text),
    p_payload
  )
  on conflict do nothing;

  get diagnostics event_inserted = row_count;
  if event_inserted = 0 then
    return query
    select event_row.id, event_row.user_id, p_provider_call_id,
      event_row.medicine_name, event_row.remaining_pills, 'none'::text;
    return;
  end if;

  case lower(p_event_name)
    when 'leg_connecting' then
      update public.refill_alert_call_attempts
      set status = 'initiated',
          initiated_at = coalesce(initiated_at, now())
      where id = attempt_row.id;

      if event_row.voice_status in ('claimed', 'provider_accepted') then
        update public.refill_alert_events
        set voice_status = 'initiated',
            call_initiated_at = coalesce(call_initiated_at, now()),
            claim_lease_expires_at = null
        where id = event_row.id;
      end if;

    when 'leg_ringing' then
      update public.refill_alert_call_attempts set status = 'ringing'
      where id = attempt_row.id;

      if event_row.voice_status in ('claimed', 'provider_accepted', 'initiated') then
        update public.refill_alert_events
        set voice_status = 'ringing',
            claim_lease_expires_at = null
        where id = event_row.id;
      end if;

    when 'leg_answered' then
      update public.refill_alert_call_attempts
      set status = 'answered',
          answered_at = coalesce(answered_at, now())
      where id = attempt_row.id;

      if event_row.voice_status in ('claimed', 'provider_accepted', 'initiated', 'ringing') then
        update public.refill_alert_events
        set voice_status = 'answered',
            call_answered_at = coalesce(call_answered_at, now()),
            claim_lease_expires_at = null
        where id = event_row.id;
        action_value := 'speak';
      end if;

    when 'say_completed' then
      if event_row.voice_status = 'answered' then
        update public.refill_alert_events
        set voice_status = 'awaiting_acknowledgement'
        where id = event_row.id;
        action_value := 'gather';
      end if;

    when 'gather_success' then
      digits := coalesce(
        p_payload #>> '{event_data,data,digits}',
        p_payload #>> '{data,digits}',
        p_payload ->> 'digits',
        ''
      );

      if digits = '1' then
        update public.refill_alert_events
        set voice_status = 'acknowledged',
            acknowledged_at = coalesce(acknowledged_at, now())
        where id = event_row.id;
      elsif digits = '9' then
        update public.profiles
        set voice_call_opted_out_at = now()
        where id = event_row.user_id;

        update public.refill_alert_events
        set voice_status = 'opted_out',
            acknowledged_at = null,
            next_retry_at = null
        where id = event_row.id;
      else
        update public.refill_alert_events
        set voice_status = 'completed'
        where id = event_row.id
          and voice_status not in ('acknowledged', 'opted_out');
      end if;

      action_value := 'hangup';

    when 'gather_failed' then
      update public.refill_alert_events
      set voice_status = 'completed'
      where id = event_row.id
        and voice_status not in ('acknowledged', 'opted_out');
      action_value := 'hangup';

    when 'say_failed_to_start', 'play_failed_to_start' then
      perform public.pillmate_mark_voice_attempt_failed(
        attempt_row.id,
        'The provider could not play the refill message.'
      );
      action_value := 'hangup';

    when 'leg_terminated' then
      terminal_reason := coalesce(
        p_payload #>> '{data,reason}',
        p_payload #>> '{event_data,data,reason}',
        p_payload ->> 'reason',
        'Call ended before acknowledgement.'
      );

      update public.refill_alert_call_attempts
      set completed_at = coalesce(completed_at, now())
      where id = attempt_row.id;

      if event_row.acknowledged_at is not null then
        update public.refill_alert_events
        set call_completed_at = coalesce(call_completed_at, now()),
            claim_lease_expires_at = null
        where id = event_row.id;
      elsif event_row.call_answered_at is not null then
        update public.refill_alert_events
        set voice_status = 'completed',
            call_completed_at = coalesce(call_completed_at, now()),
            claim_lease_expires_at = null
        where id = event_row.id
          and voice_status <> 'opted_out';
      else
        perform public.pillmate_mark_voice_attempt_failed(attempt_row.id, terminal_reason);
      end if;
  end case;

  select a, e
  into attempt_row, event_row
  from public.refill_alert_call_attempts a
  join public.refill_alert_events e on e.id = a.refill_alert_event_id
  where a.id = attempt_row.id;

  return query
  select event_row.id, event_row.user_id, p_provider_call_id,
    event_row.medicine_name, event_row.remaining_pills, action_value;
end;
$$;

create or replace function public.pillmate_mark_exotel_action_failed(
  p_provider_call_id text,
  p_failure_reason text
)
returns void
language plpgsql
security definer
set search_path = public
as $$
declare
  attempt_id_value uuid;
begin
  select id into attempt_id_value
  from public.refill_alert_call_attempts
  where provider = 'exotel'
    and provider_call_id = p_provider_call_id
  limit 1;

  if attempt_id_value is not null then
    perform public.pillmate_mark_voice_attempt_failed(attempt_id_value, p_failure_reason);
  end if;
end;
$$;

create or replace function public.schedule_pillmate_voice_retry_worker()
returns void
language plpgsql
security definer
set search_path = public
as $fn$
begin
  begin
    perform cron.unschedule('pillmate-process-refill-voice-alerts');
  exception
    when others then
      null;
  end;

  perform cron.schedule(
    'pillmate-process-refill-voice-alerts',
    '*/5 * * * *',
    $job$
      select net.http_post(
        url := (select decrypted_secret from vault.decrypted_secrets
                where name = 'pillmate_function_url'
                limit 1) || '/functions/v1/send-refill-voice-alerts',
        headers := jsonb_build_object(
          'Content-Type', 'application/json',
          'x-pillmate-function-secret',
          (select decrypted_secret from vault.decrypted_secrets
           where name = 'pillmate_function_secret'
           limit 1)
        ),
        body := '{"scan":true}'::jsonb
      );
    $job$
  );
end;
$fn$;

revoke all on table public.refill_alert_events,
  public.refill_alert_call_attempts,
  public.refill_alert_provider_events
from anon, authenticated;

revoke all on function public.claim_voice_refill_alert(uuid) from public, anon, authenticated;
revoke all on function public.pillmate_mark_voice_request_accepted(uuid, text) from public, anon, authenticated;
revoke all on function public.pillmate_mark_voice_attempt_failed(uuid, text) from public, anon, authenticated;
revoke all on function public.pillmate_mark_test_mode_skipped(uuid) from public, anon, authenticated;
revoke all on function public.list_due_voice_refill_alerts(integer) from public, anon, authenticated;
revoke all on function public.record_exotel_voice_event(text, text, jsonb) from public, anon, authenticated;
revoke all on function public.pillmate_mark_exotel_action_failed(text, text) from public, anon, authenticated;
revoke all on function public.schedule_pillmate_voice_retry_worker() from public, anon, authenticated;

grant execute on function public.claim_voice_refill_alert(uuid) to service_role;
grant execute on function public.pillmate_mark_voice_request_accepted(uuid, text) to service_role;
grant execute on function public.pillmate_mark_voice_attempt_failed(uuid, text) to service_role;
grant execute on function public.pillmate_mark_test_mode_skipped(uuid) to service_role;
grant execute on function public.list_due_voice_refill_alerts(integer) to service_role;
grant execute on function public.record_exotel_voice_event(text, text, jsonb) to service_role;
grant execute on function public.pillmate_mark_exotel_action_failed(text, text) to service_role;
grant execute on function public.schedule_pillmate_voice_retry_worker() to service_role;

-- Run this only after creating the two Vault secrets named
-- pillmate_function_url and pillmate_function_secret. It is intentionally not
-- called by the migration so that a missing secret can never affect refills.
-- select public.schedule_pillmate_voice_retry_worker();
