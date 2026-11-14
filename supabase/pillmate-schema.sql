-- PillMate medication setup: run this in Supabase SQL Editor.
-- Each row is owned by one authenticated user. RLS prevents cross-account access.

create table if not exists public.profiles (
  id uuid primary key references auth.users(id) on delete cascade,
  full_name text,
  whatsapp_phone text,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create table if not exists public.medications (
  id uuid primary key default gen_random_uuid(),
  user_id uuid not null references auth.users(id) on delete cascade,
  medicine_name text not null,
  total_pills integer not null check (total_pills > 0),
  pills_remaining integer not null check (pills_remaining >= 0 and pills_remaining <= total_pills),
  daily_dose numeric(8,2) not null check (daily_dose > 0),
  reminder_times text[] not null default '{}',
  active boolean not null default true,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

alter table public.profiles enable row level security;
alter table public.medications enable row level security;

drop policy if exists "Users can view own profile" on public.profiles;
create policy "Users can view own profile" on public.profiles for select to authenticated using (auth.uid() = id);
drop policy if exists "Users can insert own profile" on public.profiles;
create policy "Users can insert own profile" on public.profiles for insert to authenticated with check (auth.uid() = id);
drop policy if exists "Users can update own profile" on public.profiles;
create policy "Users can update own profile" on public.profiles for update to authenticated using (auth.uid() = id) with check (auth.uid() = id);

drop policy if exists "Users can view own medications" on public.medications;
create policy "Users can view own medications" on public.medications for select to authenticated using (auth.uid() = user_id);
drop policy if exists "Users can add own medications" on public.medications;
create policy "Users can add own medications" on public.medications for insert to authenticated with check (auth.uid() = user_id);
drop policy if exists "Users can update own medications" on public.medications;
create policy "Users can update own medications" on public.medications for update to authenticated using (auth.uid() = user_id) with check (auth.uid() = user_id);
drop policy if exists "Users can delete own medications" on public.medications;
create policy "Users can delete own medications" on public.medications for delete to authenticated using (auth.uid() = user_id);

create index if not exists medications_user_active_idx on public.medications(user_id, active);
