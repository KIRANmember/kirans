-- Weekly medication review: caretaker-recorded dose confirmations.
create table if not exists public.medication_dose_logs (
 id uuid primary key default gen_random_uuid(),
 user_id uuid not null references auth.users(id) on delete cascade,
 medication_id uuid not null references public.medications(id) on delete cascade,
 scheduled_at timestamptz not null,
 status text not null check (status in ('taken','missed','skipped')),
 confirmed_at timestamptz not null default now(),
 confirmed_by uuid not null references auth.users(id),
 created_at timestamptz not null default now(),
 updated_at timestamptz not null default now(),
 constraint medication_dose_logs_medication_schedule_unique unique (medication_id,scheduled_at)
);
alter table public.medication_dose_logs enable row level security;
drop policy if exists "Users can view own dose logs" on public.medication_dose_logs;
create policy "Users can view own dose logs" on public.medication_dose_logs for select to authenticated using (auth.uid()=user_id);
drop policy if exists "Users can add own dose logs" on public.medication_dose_logs;
create policy "Users can add own dose logs" on public.medication_dose_logs for insert to authenticated with check (auth.uid()=user_id and auth.uid()=confirmed_by and exists(select 1 from public.medications m where m.id=medication_id and m.user_id=auth.uid()));
drop policy if exists "Users can update own dose logs" on public.medication_dose_logs;
create policy "Users can update own dose logs" on public.medication_dose_logs for update to authenticated using (auth.uid()=user_id) with check (auth.uid()=user_id and auth.uid()=confirmed_by);
create index if not exists medication_dose_logs_user_scheduled_idx on public.medication_dose_logs(user_id,scheduled_at desc);
