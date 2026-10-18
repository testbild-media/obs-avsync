// Shared definition of the AV-Sync test signal.
// The phone page (web/app.js) implements exactly the same constants - keep both in sync.
#pragma once

#include <cstdint>
#include <vector>

namespace avs::spec {

constexpr int kProtocolVersion = 1;

// Events happen at multiples of the period on the reference wall clock.
constexpr double kDefaultPeriodMs = 1000.0;
// Every Nth event is a marker (wider flash, down-chirp) to resolve latencies > period.
constexpr int kMarkerEvery = 10;

// Video: raised-cosine (Hann) light pulse, centred on the event time.
constexpr double kPulseWidthMs = 200.0;
constexpr double kMarkerPulseWidthMs = 500.0;
// Marker detection: measured pulse sigma > ratio * typical sigma of recent pulses.
// (Hann sigma: normal 28 ms, marker 71 ms; camera gamma widens both by the same factor.)
constexpr double kMarkerSigmaRatio = 1.6;
constexpr double kNormalSigmaGuessMs = 40.0;

// Audio: linear chirp that STARTS at the event time.
constexpr double kChirpDurationMs = 60.0;
constexpr double kChirpLowHz = 800.0;
constexpr double kChirpHighHz = 4000.0;
constexpr double kChirpTaperMs = 5.0;

inline int64_t floorDiv(int64_t a, int64_t b)
{
	int64_t q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0)))
		--q;
	return q;
}

inline bool isMarker(int64_t seq)
{
	int64_t r = seq % kMarkerEvery;
	return r == 0;
}

inline double pulseWidthMs(bool marker)
{
	return marker ? kMarkerPulseWidthMs : kPulseWidthMs;
}

// Hann pulse value (0..1) at time t relative to the pulse centre.
double hann(double tRelMs, double widthMs);

// Chirp waveform (normal = up-chirp, marker = down-chirp), peak amplitude 1.
std::vector<float> makeChirp(double sampleRate, bool down);

} // namespace avs::spec
