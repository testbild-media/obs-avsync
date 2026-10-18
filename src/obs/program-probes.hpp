// Measurement on the program output: the composited main view (video) and one audio track.
#pragma once

#include "audio-detector.hpp"
#include "engine.hpp"
#include "frame-grabber.hpp"
#include "ntp-clock.hpp"
#include "video-detector.hpp"

#include <obs-module.h>

#include <memory>
#include <mutex>
#include <vector>

namespace avs {

class ProgramProbes {
public:
	ProgramProbes(Engine &engine, NtpClock &clock);
	~ProgramProbes();

	void configure(bool video, bool audio, int track);
	void stop();

private:
	static void renderedCallback(void *param);
	static void audioCallback(void *param, size_t mixIdx, struct audio_data *data);
	void onRendered();
	void onAudio(const struct audio_data *data);

	Engine &engine_;
	NtpClock &clock_;

	bool videoOn_ = false;
	bool audioOn_ = false;
	int track_ = 1;
	int videoProbe_ = 0;
	int audioProbe_ = 0;

	// graphics thread
	FrameGrabber grabber_;
	FlashDetector flash_;
	uint64_t lastFrameTime_ = 0;
	int statusCounter_ = 0;

	// audio thread
	std::unique_ptr<ChirpDetector> chirp_;
	std::vector<float> mono_;
	uint64_t lastAudioStatusNs_ = 0;
	uint64_t lastAudioDetectNs_ = 0;
};

} // namespace avs
