#pragma once

#include <cstdint>

// A sample clock for the application-driven OpenAL mixer. Callback cadence is
// allowed to jitter; sample timestamps never accumulate rounded block lengths.
// A stalled engine drops elapsed time explicitly instead of doing unbounded
// catch-up work in the sound interrupt.
class AudioCaptureClock {
public:
	static constexpr uint32_t Rate = 44100;
	static constexpr uint32_t BlockFrames = Rate / 100;
	// Below OpenAL streaming sources' three 1024-frame queued buffers.
	static constexpr uint32_t MaxBacklogFrames = Rate / 20;

	void Reset() { initialized = false; discontinuity = true; }
	uint32_t Due( uint64_t now ) {
		if ( !initialized || now < anchor ) {
			anchor = now; frames = 0; initialized = true; discontinuity = true;
			return 0;
		}
		const uint64_t elapsed = now - anchor;
		const uint64_t target = elapsed / 1000000000 * Rate + elapsed % 1000000000 * Rate / 1000000000;
		if ( target < frames || target - frames > MaxBacklogFrames ) {
			anchor = now; frames = 0; discontinuity = true;
			return 0;
		}
		return static_cast<uint32_t>( target - frames );
	}
	uint64_t Time() const {
		return anchor + frames / Rate * 1000000000 + frames % Rate * 1000000000 / Rate;
	}
	bool Discontinuity() const { return discontinuity; }
	void Consume( uint32_t count ) { frames += count; discontinuity = false; }

private:
	uint64_t anchor = 0;
	uint64_t frames = 0;
	bool initialized = false;
	bool discontinuity = true;
};
