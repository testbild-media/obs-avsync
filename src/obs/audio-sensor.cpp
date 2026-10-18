// Audio filter "AV-Sync Audio-Sensor": detects the phone's chirps in a source.
#include "audio-detector.hpp"
#include "plugin.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace avs {

namespace {

constexpr int64_t kDirectTsWindowNs = 2000000000LL;   // libobs MAX_TS_VAR

struct AudioSensor {
	obs_source_t *self = nullptr;
	int probeId = 0;
	std::string lastName;
	std::atomic<double> threshold{0.35};

	// audio thread
	std::unique_ptr<ChirpDetector> detector;
	std::vector<float> mono;
	bool haveAdjust = false;
	int64_t adjustNs = 0;
	uint64_t expectedNextTs = 0;
	uint64_t lastStatusNs = 0;
	std::atomic<uint64_t> lastDetectNs{0};
};

const char *getName(void *)
{
	return obs_module_text("AudioSensor");
}

void update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<AudioSensor *>(data);
	s->threshold = obs_data_get_double(settings, "threshold");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new AudioSensor();
	s->self = source;
	s->probeId = Plugin::instance().engine().addProbe(Kind::Audio, obs_module_text("AudioSensor"), "filter", "");
	update(s, settings);
	return s;
}

void destroy(void *data)
{
	auto *s = static_cast<AudioSensor *>(data);
	Plugin::instance().engine().removeProbe(s->probeId);
	delete s;
}

void getDefaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "threshold", 0.35);
}

obs_properties_t *getProperties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *thr = obs_properties_add_float_slider(props, "threshold",
							      obs_module_text("AudioSensor.Threshold"), 0.15, 0.9, 0.01);
	obs_property_set_long_description(thr, obs_module_text("AudioSensor.Threshold.Description"));
	obs_properties_add_text(props, "info", obs_module_text("AudioSensor.Info"), OBS_TEXT_INFO);
	return props;
}

void videoTickLikeRename(AudioSensor *s)
{
	obs_source_t *parent = obs_filter_get_parent(s->self);
	if (!parent)
		return;
	const char *name = obs_source_get_name(parent);
	if (name && s->lastName != name) {
		s->lastName = name;
		Plugin::instance().engine().setProbeName(s->probeId, name);
		Plugin::instance().engine().setProbeSource(s->probeId, obs_source_get_uuid(parent));
	}
}

struct obs_audio_data *filterAudio(void *data, struct obs_audio_data *audio)
{
	auto *s = static_cast<AudioSensor *>(data);
	if (!audio || !audio->frames)
		return audio;
	videoTickLikeRename(s);

	const uint32_t sampleRate = audio_output_get_sample_rate(obs_get_audio());
	if (!s->detector || s->detector->sampleRate() != (double)sampleRate) {
		s->detector = std::make_unique<ChirpDetector>((double)sampleRate);
		s->detector->setCallback([s](const AudioDetection &d) {
			s->lastDetectNs = os_gettime_ns();
			Plugin &p = Plugin::instance();
			p.engine().reportDetection(s->probeId, p.utcMs(d.timeNs), d.marker, d.score);
		});
	}
	s->detector->setThreshold(s->threshold);

	// mono downmix
	const size_t channels = audio_output_get_channels(obs_get_audio());
	s->mono.assign(audio->frames, 0.0f);
	size_t used = 0;
	for (size_t ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++) {
		const float *src = reinterpret_cast<const float *>(audio->data[ch]);
		if (!src)
			continue;
		for (uint32_t i = 0; i < audio->frames; i++)
			s->mono[i] += src[i];
		used++;
	}
	if (used > 1)
		for (auto &v : s->mono)
			v /= (float)used;

	// Filters see the source's own timestamps. libobs maps them to the system clock
	// (timing_adjust) only afterwards, so the same mapping is reproduced here.
	const uint64_t now = os_gettime_ns();
	const uint64_t ts = audio->timestamp;
	const int64_t diffNow = (int64_t)(ts - now);
	if (std::llabs(diffNow) < kDirectTsWindowNs) {
		s->adjustNs = 0;
		s->haveAdjust = true;
	} else if (!s->haveAdjust || std::llabs((int64_t)(ts - s->expectedNextTs)) > kDirectTsWindowNs) {
		s->adjustNs = (int64_t)(now - ts);
		s->haveAdjust = true;
	}
	s->expectedNextTs = ts + (uint64_t)((double)audio->frames * 1e9 / sampleRate);

	obs_source_t *parent = obs_filter_get_parent(s->self);
	const int64_t syncOffset = parent ? obs_source_get_sync_offset(parent) : 0;
	const uint64_t outTs = (uint64_t)((int64_t)ts + s->adjustNs + syncOffset);
	s->detector->push(s->mono.data(), audio->frames, outTs);

	if (now - s->lastStatusNs > 500000000ULL) {
		s->lastStatusNs = now;
		ProbeSignal sig;
		sig.level = s->detector->recentLevelDb();
		sig.ok = now - s->lastDetectNs.load() < 3000000000ULL;
		char buf[96];
		snprintf(buf, sizeof(buf), obs_module_text(sig.ok ? "Signal.AudioOk" : "Signal.AudioSearching"),
			 sig.level);
		sig.text = buf;
		Plugin::instance().engine().setProbeSignal(s->probeId, sig);
	}
	return audio;
}

void filterAdd(void *data, obs_source_t *parent)
{
	auto *s = static_cast<AudioSensor *>(data);
	if (!parent)
		return;
	// lastName belongs to the audio thread; only update the engine here
	const char *name = obs_source_get_name(parent);
	Plugin::instance().engine().setProbeName(s->probeId, name ? name : "");
	Plugin::instance().engine().setProbeSource(s->probeId, obs_source_get_uuid(parent));
}

} // namespace

void registerAudioSensor()
{
	obs_source_info info = {};
	info.id = "avsync_audio_sensor";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = getDefaults;
	info.get_properties = getProperties;
	info.filter_add = filterAdd;
	info.filter_audio = filterAudio;
	obs_register_source(&info);
}

} // namespace avs
