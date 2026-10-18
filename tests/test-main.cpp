// Offline tests for the AV-Sync detectors and engine (no OBS required).
#include "../src/audio-detector.hpp"
#include "../src/engine.hpp"
#include "../src/signal-spec.hpp"
#include "../src/video-detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace avs;

static int g_failures = 0;
#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			g_failures++;                                      \
			printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                               \
			printf("\n");                                      \
		}                                                          \
	} while (0)

struct ErrStats {
	std::vector<double> e;
	void add(double v) { e.push_back(v); }
	double mean() const
	{
		double s = 0;
		for (double v : e)
			s += v;
		return e.empty() ? 0 : s / e.size();
	}
	double maxAbs() const
	{
		double m = 0;
		for (double v : e)
			m = std::max(m, std::fabs(v));
		return m;
	}
};

// ---------------------------------------------------------------- audio

struct AudioCase {
	const char *name;
	double sr;
	double latencyMs;
	double noiseDb;
	bool reverb;
	bool invert;
	double gain;
};

static void testAudio(const AudioCase &c)
{
	std::mt19937 rng(1234);
	std::normal_distribution<double> noise(0.0, std::pow(10.0, c.noiseDb / 20.0));
	const double durS = 25.0;
	const size_t n = (size_t)(durS * c.sr);
	std::vector<float> sig(n, 0.0f);

	// events at t = k s (k = 0..24), timeline starts at "UTC" 1000 s
	const double t0Ms = 1'000'000.0;
	std::vector<int64_t> seqs;
	for (int64_t k = 1001; k < 1024; k++) {
		bool marker = spec::isMarker(k);
		auto chirp = spec::makeChirp(c.sr, marker);
		double startMs = (double)k * 1000.0 + c.latencyMs - t0Ms;
		double startSample = startMs / 1000.0 * c.sr;
		// fractional delay via linear interpolation of the chirp
		int64_t base = (int64_t)std::floor(startSample);
		double frac = startSample - base;
		auto add = [&](double delaySamples, double g) {
			for (size_t i = 0; i <= chirp.size(); i++) {
				double a = i < chirp.size() ? chirp[i] : 0.0;
				double b = i > 0 ? chirp[i - 1] : 0.0;
				double v = (1.0 - frac) * a + frac * b;
				int64_t idx = base + (int64_t)i + (int64_t)delaySamples;
				if (idx >= 0 && idx < (int64_t)n)
					sig[(size_t)idx] += (float)(v * g * c.gain * (c.invert ? -1 : 1));
			}
		};
		add(0, 1.0);
		if (c.reverb) {
			add(0.007 * c.sr, 0.6);
			add(0.013 * c.sr, -0.45);
			add(0.031 * c.sr, 0.3);
			add(0.047 * c.sr, -0.25);
		}
		seqs.push_back(k);
	}
	if (c.noiseDb > -150) // below: exact digital silence (browser / NDI sources)
		for (auto &v : sig)
			v += (float)noise(rng);

	ChirpDetector det(c.sr);
	std::vector<AudioDetection> found;
	det.setCallback([&](const AudioDetection &d) { found.push_back(d); });
	const size_t chunk = 1024;
	for (size_t i = 0; i < n; i += chunk) {
		size_t cnt = std::min(chunk, n - i);
		uint64_t ts = (uint64_t)((t0Ms + (double)i / c.sr * 1000.0) * 1e6);
		det.push(sig.data() + i, cnt, ts);
	}

	ErrStats err;
	int markerOk = 0;
	for (const auto &d : found) {
		double tMs = (double)d.timeNs / 1e6;
		int64_t k = (int64_t)std::llround((tMs - c.latencyMs) / 1000.0);
		double e = tMs - ((double)k * 1000.0 + c.latencyMs);
		err.add(e);
		if (d.marker == spec::isMarker(k))
			markerOk++;
	}
	printf("audio %-28s found %2zu/%2zu  err mean %+.3f ms  max %.3f ms  marker ok %d\n", c.name, found.size(),
	       seqs.size(), err.mean(), err.maxAbs(), markerOk);
	CHECK(found.size() == seqs.size(), "found %zu expected %zu", found.size(), seqs.size());
	CHECK(err.maxAbs() < 0.25, "max error %.3f ms", err.maxAbs());
	CHECK(markerOk == (int)found.size(), "marker classification %d/%zu", markerOk, found.size());
}

static void testAudioSilenceNoFalseAlarms()
{
	std::mt19937 rng(99);
	std::normal_distribution<double> noise(0.0, 0.05);
	const double sr = 48000;
	std::vector<float> sig((size_t)(30 * sr));
	for (auto &v : sig)
		v = (float)noise(rng);
	// add some speech-like tone bursts
	for (int b = 0; b < 30; b++) {
		size_t start = (size_t)(b * sr + 1000);
		double f = 200 + 37 * b;
		for (size_t i = 0; i < 9000; i++)
			sig[start + i] += (float)(0.3 * std::sin(2 * 3.14159 * f * i / sr));
	}
	ChirpDetector det(sr);
	int count = 0;
	det.setCallback([&](const AudioDetection &) { count++; });
	for (size_t i = 0; i < sig.size(); i += 480)
		det.push(sig.data() + i, std::min<size_t>(480, sig.size() - i), (uint64_t)(i / sr * 1e9) + 1);
	printf("audio noise+tones false alarms: %d\n", count);
	CHECK(count == 0, "false alarms %d", count);
}

// ---------------------------------------------------------------- video (signal level)

struct VideoCase {
	const char *name;
	double cameraFps;
	double exposureFrac; // exposure time / frame interval
	double displayHz;    // phone refresh (sample and hold)
	double outputFps;    // OBS render rate (duplicates)
	double latencyMs;    // pipeline latency (exposure centre -> appearance in OBS)
	double gamma;
	double noise;
};

// light emitted by the phone at time t (ms, UTC), 0..1, sample-and-hold at display rate
static double phoneLight(double tMs, double displayHz)
{
	double R = 1000.0 / displayHz;
	double slot = std::floor(tMs / R) * R; // presentation time of the frame being shown
	double mid = slot + R / 2;             // the page renders hann(mid - T)
	int64_t k = (int64_t)std::llround(mid / 1000.0);
	double v = spec::hann(mid - (double)k * 1000.0, spec::pulseWidthMs(spec::isMarker(k)));
	// The page pre-compensates the display gamma, so light ~ hann.
	return v;
}

static void testVideoSignal(const VideoCase &c)
{
	std::mt19937 rng(42);
	std::normal_distribution<double> noise(0.0, c.noise);
	std::uniform_real_distribution<double> phase(0.0, 1.0);

	const double t0 = 5'000'000.0; // UTC ms
	const double dt = 1000.0 / c.cameraFps;
	const double exp = c.exposureFrac * dt;
	const double outDt = 1000.0 / c.outputFps;
	double a = t0 + phase(rng) * dt; // first exposure start

	FlashDetector det;
	FlashDetectorConfig cfg;
	cfg.autoRoi = false;
	det.setConfig(cfg);
	std::vector<VideoDetection> found;
	det.setCallback([&](const VideoDetection &d) { found.push_back(d); });

	// generate camera frames; each one appears in OBS at the next output tick after
	// (exposure centre + latency) and is repeated until the next camera frame arrives.
	struct CamFrame {
		double appear;
		double value;
	};
	std::vector<CamFrame> frames;
	const double endT = t0 + 30'000.0;
	while (a < endT) {
		// integrate light over the exposure
		int steps = 40;
		double acc = 0.0;
		for (int i = 0; i < steps; i++)
			acc += phoneLight(a + exp * (i + 0.5) / steps, c.displayHz);
		double light = acc / steps;
		double v = 20.0 + 180.0 * std::pow(light, 1.0 / c.gamma) + noise(rng);
		double centre = a + exp / 2;
		frames.push_back({centre + c.latencyMs, v});
		a += dt;
	}
	// render at output rate (first appearance quantised to output ticks)
	double outT = std::ceil(frames.front().appear / outDt) * outDt;
	size_t fi = 0;
	double cur = frames.front().value;
	std::vector<std::pair<double, double>> rendered;
	double quantSum = 0.0;
	int quantN = 0;
	for (; outT < endT + 1000; outT += outDt) {
		while (fi < frames.size() && frames[fi].appear <= outT) {
			quantSum += outT - frames[fi].appear;
			quantN++;
			cur = frames[fi++].value;
		}
		rendered.push_back({outT, cur});
	}
	// FlashDetector::pushSignal has no duplicate filter (pushFrame has), emulate it here
	double prev = -1;
	for (auto &r : rendered) {
		if (r.second == prev)
			continue;
		prev = r.second;
		det.pushSignal(r.second, (uint64_t)(r.first * 1e6));
	}

	ErrStats err;
	int markerOk = 0;
	for (const auto &d : found) {
		double tMs = (double)d.timeNs / 1e6;
		int64_t k = (int64_t)std::llround((tMs - c.latencyMs) / 1000.0);
		err.add(tMs - ((double)k * 1000.0 + c.latencyMs));
		if (d.marker == spec::isMarker(k))
			markerOk++;
		if (getenv("AVSYNC_VERBOSE"))
			printf("    k=%lld sigma=%.1f amp=%.1f n=%d\n", (long long)k, d.sigmaMs, d.amplitude, d.samples);
	}
	// The output tick quantisation delays the first appearance of each camera frame.
	double expectedBias = quantN ? quantSum / quantN : 0.0;
	printf("video %-34s found %2zu  err mean %+6.2f ms (exp. %+5.2f)  max %5.2f ms  marker ok %d/%zu\n", c.name,
	       found.size(), err.mean(), expectedBias, err.maxAbs(), markerOk, found.size());
	CHECK(found.size() >= 28, "found %zu", found.size());
	// Very short exposures sample the phone's display steps at a fixed phase: up to half a refresh.
	double tol = 3.0 + (c.exposureFrac * dt < 1000.0 / c.displayHz ? 500.0 / c.displayHz : 0.0);
	CHECK(std::fabs(err.mean() - expectedBias) < tol, "mean error %.2f ms (expected %.2f)", err.mean(),
	      expectedBias);
	CHECK(err.maxAbs() < expectedBias + 12.0, "max error %.2f", err.maxAbs());
	CHECK(markerOk >= (int)found.size() - 1, "marker classification %d/%zu", markerOk, found.size());
}

// ---------------------------------------------------------------- video (frames + auto ROI)

static void testVideoAutoRoi()
{
	std::mt19937 rng(7);
	std::normal_distribution<double> noise(0.0, 2.0);
	const int W = 256, H = 144;
	std::vector<uint8_t> img((size_t)W * H * 4);
	const double latency = 120.0;
	const double t0 = 7'000'000.0;
	const double dt = 1000.0 / 30.0;

	FlashDetector det;
	std::vector<VideoDetection> found;
	det.setCallback([&](const VideoDetection &d) { found.push_back(d); });

	// phone at x 170..210, y 30..100 ; a "person" (bright blob) moves around randomly
	for (int f = 0; f < 30 * 40; f++) {
		double t = t0 + f * dt;
		double light = phoneLight(t - latency, 60.0);
		double bx = 60 + 40 * std::sin(f * 0.07), by = 70 + 30 * std::cos(f * 0.05);
		for (int y = 0; y < H; y++) {
			for (int x = 0; x < W; x++) {
				double v = 60 + 0.3 * x;
				double d2 = (x - bx) * (x - bx) + (y - by) * (y - by);
				if (d2 < 900)
					v += 90;
				if (x >= 170 && x < 210 && y >= 30 && y < 100)
					v = 15 + 200 * light;
				// distractor: tenths digit of a clock (periodic with 1 s, step shaped)
				if (x >= 170 && x < 210 && y >= 112 && y < 140) {
					double ms = std::fmod(t - latency, 1000.0);
					v = 40 + 18 * std::floor(ms / 100.0);
				}
				v += noise(rng);
				uint8_t q = (uint8_t)std::clamp(v, 0.0, 255.0);
				uint8_t *p = &img[((size_t)y * W + x) * 4];
				p[0] = p[1] = p[2] = q;
				p[3] = 255;
			}
		}
		det.pushFrame(img.data(), W, H, W * 4, (uint64_t)(t * 1e6));
	}
	auto st = det.status();
	ErrStats err;
	for (const auto &d : found) {
		double tMs = (double)d.timeNs / 1e6;
		int64_t k = (int64_t)std::llround((tMs - latency) / 1000.0);
		err.add(tMs - ((double)k * 1000.0 + latency));
	}
	printf("video auto-ROI: locked %d bbox (%.2f,%.2f)-(%.2f,%.2f) found %zu err mean %+.2f max %.2f\n", st.locked,
	       st.bboxX0, st.bboxY0, st.bboxX1, st.bboxY1, found.size(), err.mean(), err.maxAbs());
	CHECK(st.locked, "ROI not locked");
	CHECK(st.bboxX0 >= 0.64 && st.bboxX1 <= 0.84 && st.bboxY0 >= 0.18 && st.bboxY1 <= 0.72, "bbox wrong");
	CHECK(found.size() >= 30, "found %zu", found.size());
	CHECK(err.maxAbs() < 6.0, "max err %.2f", err.maxAbs());
}

// ---------------------------------------------------------------- engine

static void testEngineLongLatency()
{
	double now = 0;
	Engine eng([&] { return now; });
	eng.setMaxLatencyMs(5000);
	int v = eng.addProbe(Kind::Video, "Kamera", "filter", "");
	int a = eng.addProbe(Kind::Audio, "Mikro", "filter", "");
	const double latV = 1350.0, latA = 1290.0;
	// start mid-way so that the first marker arrives after a few events
	for (int64_t k = 2003; k < 2040; k++) {
		now = k * 1000.0 + 2000;
		eng.reportDetection(v, k * 1000.0 + latV, spec::isMarker(k), 1.0);
		eng.reportDetection(a, k * 1000.0 + latA, spec::isMarker(k), 1.0);
	}
	auto snap = eng.snapshot(100);
	bool allOk = true;
	for (const auto &p : snap) {
		double expect = p.kind == Kind::Video ? latV : latA;
		for (const auto &m : p.recent)
			if (std::fabs(m.latencyMs - expect) > 1e-6)
				allOk = false;
		printf("engine probe %-7s live mean %.1f n=%d locked=%d\n", p.name.c_str(), p.live.mean, p.total.count,
		       p.locked);
	}
	CHECK(allOk, "latencies not remapped after marker lock");
	auto ps = eng.pair(v, a, 50);
	printf("engine pair offset mean %.1f n=%d\n", ps.offset.mean, ps.offset.count);
	CHECK(std::fabs(ps.offset.mean - (latA - latV)) < 1e-6, "pair offset %.2f", ps.offset.mean);
}

static void testEngineMisclassifiedMarker()
{
	double now = 0;
	Engine eng([&] { return now; });
	eng.setMaxLatencyMs(5000);
	int v = eng.addProbe(Kind::Video, "Kamera", "filter", "");
	const double lat = 1350.0;
	int bad = 0;
	for (int64_t k = 3005; k < 3060; k++) {
		bool flag = spec::isMarker(k) || k == 3033 || k == 3047; // two normal pulses look like markers
		auto m = eng.reportDetection(v, k * 1000.0 + lat, flag, 1.0);
		// after the confirmed lock (second marker) every new result must be right immediately
		if (k > 3020 && (m.seq != k || std::fabs(m.latencyMs - lat) > 1e-6))
			bad++;
	}
	// earlier results are corrected retroactively
	for (const auto &p : eng.snapshot(100))
		for (const auto &m : p.recent)
			if (std::fabs(m.latencyMs - lat) > 1e-6)
				bad++;
	printf("engine misclassified markers: wrong results after lock = %d\n", bad);
	CHECK(bad == 0, "%d wrong results", bad);
}

static void testEngineSession()
{
	double now = 100'000.0;
	Engine eng([&] { return now; });
	eng.setMaxLatencyMs(1000);
	int v = eng.addProbe(Kind::Video, "Kamera", "filter", "");
	int a = eng.addProbe(Kind::Audio, "Mikro; extern", "filter", "");
	eng.setSessionPair(v, a);
	eng.scheduleSession(105'000.0, 10'000.0, "Test");
	bool finished = false;
	for (int64_t k = 101; k < 120; k++) {
		now = k * 1000.0 + 300;
		eng.reportDetection(v, k * 1000.0 + 80.0 + (k % 3), spec::isMarker(k), 1.0);
		eng.reportDetection(a, k * 1000.0 + 20.0, spec::isMarker(k), 1.0);
		finished |= eng.tick();
	}
	auto res = eng.session();
	printf("engine session finished=%d state=%d events=%lld video n=%d mean %.2f offset %.2f\n", finished,
	       (int)res.state, (long long)(res.seqEnd - res.seqBegin), res.probes[0].stats.count,
	       res.probes[0].stats.mean, res.pairOffset.mean);
	CHECK(finished, "session did not finish");
	CHECK(res.probes[0].stats.count == 10, "count %d", res.probes[0].stats.count);
	std::string csv = Engine::sessionCsv(res, true);
	CHECK(csv.find("\"Audio: Mikro; extern [ms]\"") != std::string::npos, "csv escaping");
	CHECK(csv.find("-60,") != std::string::npos || csv.find("-61,") != std::string::npos, "csv german decimals");
}

int main()
{
	printf("== audio ==\n");
	testAudio({"48k clean", 48000, 37.25, -60, false, false, 0.5});
	testAudio({"48k noise -30dB + reverb", 48000, 112.6, -30, true, false, 0.3});
	testAudio({"44.1k inverted + reverb", 44100, 5.03, -40, true, true, 0.2});
	testAudio({"48k quiet chirp -40 dBFS", 48000, 250.4, -55, true, false, 0.01});
	testAudio({"48k digital silence", 48000, 32.3, -200, false, false, 0.5});
	testAudio({"44.1k digital silence + reverb", 44100, 77.7, -200, true, false, 0.3});
	testAudioSilenceNoFalseAlarms();

	printf("== video ==\n");
	testVideoSignal({"30fps exp 0.5 disp60 out60", 30, 0.5, 60, 60, 83.4, 2.2, 1.0});
	testVideoSignal({"30fps exp 0.05 disp60 out60", 30, 0.05, 60, 60, 83.4, 2.2, 1.0});
	testVideoSignal({"30fps exp 1.0 disp120 out60", 30, 1.0, 120, 60, 140.0, 2.2, 1.0});
	testVideoSignal({"25fps exp 0.5 disp60 out50", 25, 0.5, 60, 50, 60.0, 2.2, 1.5});
	testVideoSignal({"29.97fps exp 0.3 disp60 out59.94", 29.97, 0.3, 60, 59.94, 95.0, 1.0, 1.0});
	testVideoSignal({"60fps exp 0.5 disp120 out60", 60, 0.5, 120, 60, 45.0, 2.2, 1.0});
	testVideoSignal({"15fps exp 0.5 disp60 out30", 15, 0.5, 60, 30, 200.0, 2.2, 1.0});
	testVideoSignal({"24fps exp 0.5 disp60 out60", 24, 0.5, 60, 60, 70.0, 2.2, 2.0});
	testVideoAutoRoi();

	printf("== engine ==\n");
	testEngineLongLatency();
	testEngineMisclassifiedMarker();
	testEngineSession();

	printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "ALL TESTS PASSED", g_failures);
	return g_failures ? 1 : 0;
}
