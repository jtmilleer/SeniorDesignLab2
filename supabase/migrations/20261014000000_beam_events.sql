-- Every beam state change the ESP32 reports, whether or not the email went out.
create table public.beam_events (
  id bigint generated always as identity primary key,
  created_at timestamptz not null default now(),
  event text not null check (event in ('BREACH', 'CLEAR')),
  level real not null,
  threshold real not null,
  device text not null,
  emailed boolean not null
);

-- No policies: only the beam-alert edge function (service role) reads or writes this table.
alter table public.beam_events enable row level security;
