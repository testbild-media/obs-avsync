#include "signal-spec.hpp"

#include <cmath>

namespace avs::spec {

static constexpr double kPi = 3.14159265358979323846;

double hann(double tRelMs, double widthMs)
{
	double half = widthMs * 0.5;
	if (tRelMs <= -half || tRelMs >= half)
		return 0.0;
	return 0.5 * (1.0 + std::cos(2.0 * kPi * tRelMs / widthMs));
}

std::vector<float> makeChirp(double sampleRate, bool down)
{
	const double dur = kChirpDurationMs / 1000.0;
	const double taper = kChirpTaperMs / 1000.0;
	const double f0 = down ? kChirpHighHz : kChirpLowHz;
	const double f1 = down ? kChirpLowHz : kChirpHighHz;
	const size_t n = (size_t)std::llround(dur * sampleRate);

	std::vector<float> out(n);
	for (size_t i = 0; i < n; i++) {
		double t = (double)i / sampleRate;
		double phase = 2.0 * kPi * (f0 * t + (f1 - f0) * t * t / (2.0 * dur));
		double w = 1.0;
		if (t < taper)
			w = 0.5 * (1.0 - std::cos(kPi * t / taper));
		else if (t > dur - taper)
			w = 0.5 * (1.0 - std::cos(kPi * (dur - t) / taper));
		out[i] = (float)(std::sin(phase) * w);
	}
	return out;
}

} // namespace avs::spec
