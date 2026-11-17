-- PillStrap X device integration.
-- Apply after existing PillMate schema/migrations.

create table if not exists public.wearable_devices (
  id uuid primary key default gen_random_uuid(),
  user_id uuid not null references auth.users(id) on delete cascade,
  device_code text not null unique,
  device_name text not null default 'PillStrap X',
  device_secret_hash text not null,
  firmware_version text not null default '2.0.0',
  connection_status text not null default 'offline'
    check (connection_status in ('online','offline','unstable')),
  last_seen_at timestamptz,
  last_battery_percent smallint check (
    last_battery_percent is null or last_battery_percent between 0 and 100
  ),
  is_active boolean not null default true,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.wearable_events (
  id uuid primary key default gen_random_uuid(),
  device_id uuid not null references public.wearable_devices(id) on delete cascade,
  user_id uuid not null references auth.users(id) on delete cascade,
  event_id text not null,
  event_type text not null check (event_type in (
    'heartbeat','dose_alert_received','reminder_acknowledged',
    'battery_update','connection_lost','connection_restored',
    'device_restarted','test_alert'
  )),
  medication_event_id uuid,
  details jsonb not null default '{}'::jsonb,
  occurred_at timestamptz not null,
  received_at timestamptz not null default now(),
  unique (device_id, event_id)
);

create index if not exists wearable_devices_user_idx
  on public.wearable_devices(user_id, is_active);
create index if not exists wearable_events_user_received_idx
  on public.wearable_events(user_id, received_at desc);
create index if not exists wearable_events_device_received_idx
  on public.wearable_events(device_id, received_at desc);

alter table public.wearable_devices enable row level security;
alter table public.wearable_events enable row level security;

drop policy if exists "Users can view own wearable devices" on public.wearable_devices;
create policy "Users can view own wearable devices"
  on public.wearable_devices for select to authenticated
  using (auth.uid() = user_id);

drop policy if exists "Users can view own wearable events" on public.wearable_events;
create policy "Users can view own wearable events"
  on public.wearable_events for select to authenticated
  using (auth.uid() = user_id);

-- No browser-side inserts or updates: device ingestion uses a server-side
-- Edge Function with a per-device secret and the service role key.
alter publication supabase_realtime add table public.wearable_devices;
alter publication supabase_realtime add table public.wearable_events;
