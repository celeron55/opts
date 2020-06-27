static unsigned long timestamp_age(unsigned long timestamp_ms)
{
	return millis() - timestamp_ms;
}

static bool ENM_compare_and_update(unsigned long &t0, const unsigned long &interval)
{
	bool trigger_now = timestamp_age(t0) >= interval;
	if(trigger_now)
		t0 = millis();
	return trigger_now;
}

#define EVERY_N_MILLISECONDS(ms) for(static unsigned long t0 = 0; ENM_compare_and_update(t0, ms); )


