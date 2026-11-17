# PillStrap X integration

This integration adds a per-user wearable registry and authenticated event ingestion.

## Architecture

PillStrap C3 -> ESP-NOW -> ESP32-S3 gateway -> HTTPS POST to Supabase Edge Function -> wearable_events / wearable_devices -> Supabase Realtime -> /dashboard/.

The ESP32 must never contain the Supabase service-role key. Give each device a unique random secret and store only its SHA-256 hash in wearable_devices.device_secret_hash.

## Apply the database migration

Run `supabase/migrations/20261117_pillstrap_devices.sql` in the Supabase SQL Editor.

## Deploy the Edge Function

Deploy `supabase/functions/pillstrap-ingest/index.ts` as `pillstrap-ingest`. It uses the platform-provided SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY secrets.

The endpoint accepts POST JSON:

```json
{
  "device_code": "PSX-001",
  "event_id": "boot-3-heartbeat-00001",
  "event_type": "heartbeat",
  "occurred_at": "2026-11-17T08:00:00.000Z",
  "firmware_version": "2.0.0",
  "battery_percent": 86,
  "details": {}
}
```

Header: `x-pillstrap-secret: <unique-device-secret>`.

## Enrol a device

Generate a random secret of at least 32 characters. Insert a row in wearable_devices with:
- user_id: the authenticated caregiver/patient account UUID
- device_code: a unique identifier, e.g. PSX-001
- device_secret_hash: lowercase SHA-256 hex of the random secret
- device_name: PillStrap X

Store the raw secret securely on the ESP32-S3 gateway; never commit it to Git.

## Dashboard

The caregiver dashboard reads devices/events under the signed-in user's RLS context and subscribes to Supabase Realtime. It displays connection/heartbeat, battery when supplied by hardware, and an activity feed.

## Important semantics

A `reminder_acknowledged` event means the physical confirmation button was pressed. It is not evidence that medication was swallowed. The event history is append-only and idempotent by (device_id, event_id).

## Current hardware boundary

The existing C3 firmware sends WATCH_PING and WATCH_CONFIRM over ESP-NOW. The gateway firmware must translate those into HTTPS events. Until that S3 gateway update is flashed and a device is enrolled, the dashboard will show an empty device state.
