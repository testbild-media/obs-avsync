#include "program-probes.hpp"

#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace avs {

ProgramProbes::ProgramProbes(Engine &engine, NtpClock &clock) : engine_(engine), clock_(clock)
{
	flash_.setCallback([this](const VideoDetection &d) {
		engine_.reportDetection(videoProbe_, clock_.utcMsAt(d.timeNs), d.marker, d.amplitude);
	});
}

ProgramProbes::~ProgramProbes()
{
	stop();
}

void ProgramProbes::stop()
{
	configure(false, false, track_);
}

void ProgramProbes::configure(bool video, bool audio, int track)
{
	track = std::clamp(track, 1, (int)MAX_AUDIO_MIXES);

	if (video != videoOn_) {
		if (video) {
			videoProbe_ = engine_.addProbe(Kind::Video, obs_module_text("Probe.ProgramVideo"), "program", "");
			flash_.reset();
			lastFrameTime_ = 0;
			obs_add_main_rendered_callback(renderedCallback, this);
		} else {
			obs_remove_main_rendered_callback(renderedCallback, this);
			obs_enter_graphics();
			grabber_.release();
			obs_leave_graphics();
			engine_.removeProbe(videoProbe_);
			videoProbe_ = 0;
		}
		videoOn_ = video;
	}

	if (audio != audioOn_ || (audio && track != track_)) {
		if (audioOn_) {
			obs_remove_raw_audio_callback((size_t)(track_ - 1), audioCallback, this);
			engine_.removeProbe(audioProbe_);
			audioProbe_ = 0;
			chirp_.reset();
		}
		if (audio) {
			const double sr = (double)audio_output_get_sample_rate(obs_get_audio());
			chirp_ = std::make_unique<ChirpDetector>(sr > 0 ? sr : 48000.0);
			char name[128];
			snprintf(name, sizeof(name), obs_module_text("Probe.ProgramAudio"), track);
			audioProbe_ = engine_.addProbe(Kind::Audio, name, "program", "");
			chirp_->setCallback([this](const AudioDetection &d) {
				lastAudioDetectNs_ = os_gettime_ns();
				engine_.reportDetection(audioProbe_, clock_.utcMsAt(d.timeNs), d.marker, d.score);
			});
			obs_add_raw_audio_callback((size_t)(track - 1), nullptr, audioCallback, this);
		}
		audioOn_ = audio;
	}
	track_ = track;
}

void ProgramProbes::renderedCallback(void *param)
{
	static_cast<ProgramProbes *>(param)->onRendered();
}

void ProgramProbes::audioCallback(void *param, size_t, struct audio_data *data)
{
	static_cast<ProgramProbes *>(param)->onAudio(data);
}

void ProgramProbes::onRendered()
{
	gs_texture_t *main = obs_get_main_texture();
	// The callback fires for every canvas; only analyse the main program view.
	if (!main || gs_get_render_target() != main)
		return;
	const uint64_t t = obs_get_video_frame_time();
	if (t == lastFrameTime_)
		return;
	lastFrameTime_ = t;

	const uint32_t cx = gs_texture_get_width(main);
	const uint32_t cy = gs_texture_get_height(main);
	grabber_.capture(cx, cy, t, [&] {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), main);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(main, 0, cx, cy);
	});
	grabber_.collect([this](const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t ls, uint64_t ts) {
		flash_.pushFrame(rgba, w, h, ls, ts);
	});

	if (++statusCounter_ >= 30) {
		statusCounter_ = 0;
		FlashDetectorStatus st = flash_.status();
		ProbeSignal sig;
		sig.ok = st.locked;
		sig.level = st.amplitude;
		sig.text = st.locked ? obs_module_text("Signal.DisplayFound") : obs_module_text("Signal.SearchingDisplay");
		engine_.setProbeSignal(videoProbe_, sig);
	}
}

void ProgramProbes::onAudio(const struct audio_data *data)
{
	if (!chirp_ || !data || !data->frames)
		return;
	const size_t channels = audio_output_get_channels(obs_get_audio());
	const uint32_t frames = data->frames;
	mono_.assign(frames, 0.0f);
	size_t used = 0;
	for (size_t ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++) {
		const float *src = reinterpret_cast<const float *>(data->data[ch]);
		if (!src)
			continue;
		for (uint32_t i = 0; i < frames; i++)
			mono_[i] += src[i];
		used++;
	}
	if (used > 1)
		for (auto &v : mono_)
			v /= (float)used;
	chirp_->push(mono_.data(), frames, data->timestamp);

	const uint64_t now = os_gettime_ns();
	if (now - lastAudioStatusNs_ > 500000000ULL) {
		lastAudioStatusNs_ = now;
		ProbeSignal sig;
		sig.level = chirp_->recentLevelDb();
		sig.ok = now - lastAudioDetectNs_ < 3000000000ULL;
		char buf[96];
		snprintf(buf, sizeof(buf), obs_module_text(sig.ok ? "Signal.AudioOk" : "Signal.AudioSearching"),
			 sig.level);
		sig.text = buf;
		engine_.setProbeSignal(audioProbe_, sig);
	}
}

} // namespace avs
