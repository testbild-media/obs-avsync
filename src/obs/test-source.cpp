// Source "AV-Sync Testsignal": generates the same flash + chirp signal as the phone page,
// exactly on the reference clock, plus a 7-segment clock. Used for self tests and for
// measuring chains behind OBS (e.g. stream -> remote OBS).
#include "plugin.hpp"
#include "signal-spec.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <ctime>
#include <thread>
#include <vector>

namespace avs {

namespace {

constexpr uint64_t kLeadNs = 60000000ULL; // 60 ms

struct TestSource {
	obs_source_t *self = nullptr;
	std::atomic<uint32_t> width{1280}, height{720};
	std::atomic<float> gain{0.25f};
	std::atomic<bool> showClock{true};
	std::atomic<bool> stop{false};
	std::thread audioThread;
};

const char *getName(void *)
{
	return obs_module_text("TestSource");
}

void update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<TestSource *>(data);
	s->width = (uint32_t)std::max<long long>(64, obs_data_get_int(settings, "width"));
	s->height = (uint32_t)std::max<long long>(64, obs_data_get_int(settings, "height"));
	s->gain = (float)std::pow(10.0, obs_data_get_double(settings, "volume_db") / 20.0);
	s->showClock = obs_data_get_bool(settings, "show_clock");
}

void audioLoop(TestSource *s)
{
	const uint32_t sr = audio_output_get_sample_rate(obs_get_audio());
	const std::vector<float> up = spec::makeChirp(sr, false);
	const std::vector<float> down = spec::makeChirp(sr, true);
	const uint32_t chunk = sr / 100;
	std::vector<float> buf(chunk);
	const uint64_t start = os_gettime_ns();
	uint64_t produced = 0;
	Plugin &plugin = Plugin::instance();
	const double period = plugin.engine().periodMs();

	while (!s->stop) {
		// Deliver audio ahead of time: libobs shifts a source's whole buffer when its data is
		// incomplete at mix time, which would add latency to the reference signal itself.
		const uint64_t now = os_gettime_ns() + kLeadNs;
		const uint64_t due = (uint64_t)((double)(now - start) * sr / 1e9);
		while (produced + chunk <= due && !s->stop) {
			const uint64_t ts = start + (uint64_t)((double)produced * 1e9 / sr);
			const double utc0 = plugin.utcMs(ts);
			const float gain = s->gain;
			for (uint32_t i = 0; i < chunk; i++) {
				const double utc = utc0 + (double)i * 1000.0 / sr;
				const int64_t k = (int64_t)std::floor(utc / period);
				const double pos = (utc - (double)k * period) * sr / 1000.0; // samples since chirp start
				const std::vector<float> &c = spec::isMarker(k) ? down : up;
				float v = 0.0f;
				if (pos >= 0.0 && pos < (double)c.size() - 1) {
					const size_t idx = (size_t)pos;
					const float frac = (float)(pos - (double)idx);
					v = c[idx] * (1.0f - frac) + c[idx + 1] * frac;
				}
				buf[i] = v * gain;
			}
			obs_source_audio audio = {};
			audio.data[0] = reinterpret_cast<const uint8_t *>(buf.data());
			audio.frames = chunk;
			audio.speakers = SPEAKERS_MONO;
			audio.format = AUDIO_FORMAT_FLOAT;
			audio.samples_per_sec = sr;
			audio.timestamp = ts;
			obs_source_output_audio(s->self, &audio);
			produced += chunk;
		}
		os_sleep_ms(4);
	}
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new TestSource();
	s->self = source;
	update(s, settings);
	s->audioThread = std::thread(audioLoop, s);
	return s;
}

void destroy(void *data)
{
	auto *s = static_cast<TestSource *>(data);
	s->stop = true;
	if (s->audioThread.joinable())
		s->audioThread.join();
	delete s;
}

void getDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
	obs_data_set_default_double(settings, "volume_db", -12.0);
	obs_data_set_default_bool(settings, "show_clock", true);
}

obs_properties_t *getProperties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", obs_module_text("TestSource.Width"), 64, 7680, 1);
	obs_properties_add_int(props, "height", obs_module_text("TestSource.Height"), 64, 4320, 1);
	obs_property_t *vol =
		obs_properties_add_float_slider(props, "volume_db", obs_module_text("TestSource.Volume"), -60.0, 0.0, 0.5);
	obs_property_float_set_suffix(vol, " dB");
	obs_properties_add_bool(props, "show_clock", obs_module_text("TestSource.ShowClock"));
	obs_properties_add_text(props, "info", obs_module_text("TestSource.Info"), OBS_TEXT_INFO);
	return props;
}

uint32_t getWidth(void *data)
{
	return static_cast<TestSource *>(data)->width;
}

uint32_t getHeight(void *data)
{
	return static_cast<TestSource *>(data)->height;
}

// ---------------------------------------------------------------- drawing helpers (solid effect active)

void rect(float x, float y, float w, float h)
{
	if (w <= 0 || h <= 0)
		return;
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	gs_draw_sprite(nullptr, 0, (uint32_t)std::lround(w), (uint32_t)std::lround(h));
	gs_matrix_pop();
}

void digit(int d, float x, float y, float w, float h, float t)
{
	static const uint8_t segs[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F}; // gfedcba
	const uint8_t m = segs[d % 10];
	const float vs = (h - 3 * t) / 2;
	if (m & 0x01) rect(x + t, y, w - 2 * t, t);              // a
	if (m & 0x02) rect(x + w - t, y + t, t, vs);             // b
	if (m & 0x04) rect(x + w - t, y + 2 * t + vs, t, vs);    // c
	if (m & 0x08) rect(x + t, y + h - t, w - 2 * t, t);      // d
	if (m & 0x10) rect(x, y + 2 * t + vs, t, vs);            // e
	if (m & 0x20) rect(x, y + t, t, vs);                     // f
	if (m & 0x40) rect(x + t, y + t + vs, w - 2 * t, t);     // g
}

void setColor(gs_eparam_t *param, float r, float g, float b)
{
	struct vec4 c;
	vec4_set(&c, r, g, b, 1.0f);
	gs_effect_set_vec4(param, &c);
}

void drawClock(double utcMs, float x0, float y0, float bandW, float bandH)
{
	// local time HH:MM:SS.mmm
	double secs = std::floor(utcMs / 1000.0);
	int ms = (int)(utcMs - secs * 1000.0);
	time_t tt = (time_t)secs;
	struct tm lt;
#ifdef _WIN32
	localtime_s(&lt, &tt);
#else
	localtime_r(&tt, &lt);
#endif
	const int digits[9] = {lt.tm_hour / 10, lt.tm_hour % 10, lt.tm_min / 10, lt.tm_min % 10, lt.tm_sec / 10,
			       lt.tm_sec % 10, ms / 100, (ms / 10) % 10, ms % 10};
	// layout: 9 digits, 2 colons, 1 dot
	const float h = bandH * 0.62f;
	const float w = h * 0.52f;
	const float gap = w * 0.28f;
	const float sep = w * 0.45f;
	const float t = std::max(2.0f, h * 0.11f);
	const float total = 9 * w + 8 * gap + 3 * sep;
	float x = x0 + (bandW - total) / 2;
	const float y = y0 + (bandH - h) / 2;
	for (int i = 0; i < 9; i++) {
		digit(digits[i], x, y, w, h, t);
		x += w + gap;
		if (i == 1 || i == 3) {
			rect(x + (sep - t) / 2 - gap / 2, y + h * 0.28f, t, t);
			rect(x + (sep - t) / 2 - gap / 2, y + h * 0.68f, t, t);
			x += sep;
		} else if (i == 5) {
			rect(x + (sep - t) / 2 - gap / 2, y + h - t, t, t);
			x += sep;
		}
	}
}

void videoRender(void *data, gs_effect_t *)
{
	auto *s = static_cast<TestSource *>(data);
	const float W = (float)s->width, H = (float)s->height;
	const bool clock = s->showClock;
	const float flashH = clock ? std::floor(H * 0.75f) : H;

	Plugin &plugin = Plugin::instance();
	const double utc = plugin.utcMs(obs_get_video_frame_time());
	const double period = plugin.engine().periodMs();
	const int64_t k = (int64_t)std::llround(utc / period);
	const float v = (float)spec::hann(utc - (double)k * period, spec::pulseWidthMs(spec::isMarker(k)));

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	while (gs_effect_loop(solid, "Solid")) {
		setColor(color, v, v, v);
		rect(0, 0, W, flashH);
		if (clock) {
			setColor(color, 0.02f, 0.02f, 0.02f);
			rect(0, flashH, W, H - flashH);
			setColor(color, 0.75f, 0.78f, 0.82f);
			drawClock(utc, 0, flashH, W, H - flashH);
		}
	}
}

} // namespace

void registerTestSource()
{
	obs_source_info info = {};
	info.id = "avsync_test_signal";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = getDefaults;
	info.get_properties = getProperties;
	info.get_width = getWidth;
	info.get_height = getHeight;
	info.video_render = videoRender;
	info.icon_type = OBS_ICON_TYPE_CUSTOM;
	obs_register_source(&info);
}

} // namespace avs
