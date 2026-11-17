-- Keep trigger-only SECURITY DEFINER helpers out of the public API.
revoke all on function public.detect_low_stock() from public, anon, authenticated;
revoke all on function public.pillmate_create_low_stock_refill_event() from public, anon, authenticated;
revoke all on function public.pillmate_enqueue_voice_refill_alert() from public, anon, authenticated;
revoke all on function public.pillmate_initialize_voice_refill_alert() from public, anon, authenticated;
revoke all on function public.pillmate_refresh_voice_consent_events() from public, anon, authenticated;
