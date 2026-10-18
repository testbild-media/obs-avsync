// Matched-filter detector for the AV-Sync chirps (up = normal event, down = marker).
#pragma once

#include "fft.hpp"

#include <complex>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace avs {

struct AudioDetection {
	uint64_t timeNs = 0; // time of the chirp START in the caller's timestamp domain
	bool marker = false; // down-chirp
	double score = 0.0;  // normalised cross-correlation 0..1
	double levelDb = -100.0; // RMS of the received chirp window (dBFS)
};

class ChirpDetector {
public:
	using Callback = std::function<void(const AudioDetection &)>;

	explicit ChirpDetector(double sampleRate);

	double sampleRate() const { return sampleRate_; }
	void setThreshold(double ncc) { threshold_ = ncc; }
	double threshold() const { return threshold_; }
	void setCallback(Callback cb) { callback_ = std::move(cb); }

	// Feed mono samples. timestampNs is the time of samples[0].
	void push(const float *samples, size_t count, uint64_t timestampNs);

	// Reset internal state (e.g. after a long timestamp gap).
	void reset();

	// Level of the input during the last second (dBFS, peak RMS of 10 ms blocks).
	double recentLevelDb() const { return recentLevelDb_; }
	// Highest NCC seen during the last second (useful as "signal quality" display).
	double recentBestScore() const { return recentBest_; }

private:
	struct Segment {
		int64_t startIndex;
		uint64_t timestampNs;
	};

	void processBlock();
	void handleScore(int64_t index, float up, float down, double energy);
	void finalizeCandidate();
	uint64_t indexToNs(double index) const;

	double sampleRate_;
	double threshold_ = 0.35;
	Callback callback_;

	size_t tplLen_;
	size_t fftSize_;
	size_t hop_;
	Fft fft_;
	std::vector<std::complex<float>> tplSpecUp_, tplSpecDown_;
	double tplEnergyUp_ = 0.0, tplEnergyDown_ = 0.0;

	// Input history. buffer_[0] corresponds to absolute index bufStart_.
	std::vector<float> buffer_;
	int64_t bufStart_ = 0;
	int64_t totalSamples_ = 0;
	int64_t nextBlockStart_ = 0;
	std::deque<Segment> segments_;
	uint64_t expectedNextTs_ = 0;

	std::vector<std::complex<float>> work_, workUp_, workDown_;

	// Candidate peak tracking
	struct ScorePoint {
		int64_t index;
		float up, down;
		double energy;
	};
	bool inCandidate_ = false;
	int64_t candidateEnd_ = 0;
	std::vector<ScorePoint> candidate_;
	ScorePoint prevPoint_{-1, 0, 0, 0};
	int64_t refractoryUntil_ = 0;

	// level metering
	double levelAcc_ = 0.0;
	size_t levelCount_ = 0;
	double levelPeak_ = 0.0;
	size_t levelBlocks_ = 0;
	double recentLevelDb_ = -100.0;
	double bestAcc_ = 0.0;
	double recentBest_ = 0.0;
	int64_t bestWindowStart_ = 0;
};

} // namespace avs
