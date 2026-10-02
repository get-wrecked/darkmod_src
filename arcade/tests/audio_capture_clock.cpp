// Standalone regression: c++ -std=c++17 arcade/tests/audio_capture_clock.cpp -o /tmp/audio-clock && /tmp/audio-clock
#include "../../sound/AudioCaptureClock.h"
#include <cassert>
#include <algorithm>
#include <iostream>

int main() {
	AudioCaptureClock clock;
	const uint64_t epoch = 1790956800000000000ULL;
	assert( clock.Due( epoch ) == 0 );
	uint64_t frames = 0;
	uint64_t now = epoch;
	// Variable async cadence including fractional 44.1 kHz buffer lengths.
	const uint64_t intervals[] = { 16000001, 17000119, 8100131, 25100003 };
	for ( int i = 0; i < 100000; ++i ) {
		now += intervals[i % 4];
		uint32_t due = clock.Due( now );
		assert( due > 0 && due <= AudioCaptureClock::MaxBacklogFrames );
		while ( due ) {
			const uint32_t count = std::min( due, AudioCaptureClock::BlockFrames );
			assert( clock.Time() == epoch + frames / 44100 * 1000000000 + frames % 44100 * 1000000000 / 44100 );
			assert( clock.Discontinuity() == ( frames == 0 ) );
			clock.Consume( count ); frames += count; due -= count;
		}
		assert( clock.Due( now ) == 0 );
		assert( clock.Time() <= now && now - clock.Time() <= 22676 );
	}
	// A one-second stall cannot generate a second of stale PCM or backdate
	// its replacement. The following block exposes the gap to the consumer.
	now += 1000000000;
	assert( clock.Due( now ) == 0 );
	assert( clock.Time() == now && clock.Discontinuity() );
	assert( clock.Due( now + 10000000 ) == 441 );
	clock.Consume( 441 );
	assert( clock.Time() == now + 10000000 );
	// Exact backlog bound, backwards clock, and reattachment to another epoch.
	assert( clock.Due( now + 60000000 ) == 2205 );
	assert( clock.Due( now - 1 ) == 0 && clock.Discontinuity() );
	clock.Reset();
	assert( clock.Due( epoch + 5000000000 ) == 0 );
	assert( clock.Time() == epoch + 5000000000 && clock.Discontinuity() );
	std::cout << "audio clock: 100000 variable callbacks, source gaps and restart passed\n";
}
