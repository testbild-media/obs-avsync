#include "audio-detector.hpp"
#include "signal-spec.hpp"

#include <algorithm>
#include <cmath>

namespace avs {

namespace {

constexpr double kCandidateWindowMs = 30.0;
constexpr double kRefractoryMs = 300.0;
constexpr double kDirectPathRatio = 0.6;
constexpr double kSmoothingThresholdNs = 70e6; // same as libobs TS_SMOOTHING_THRESHOLD
constexpr double kResetGapNs = 1e9;
constexpr double kMinHalfEnergyShare = 0.2;

size_t nextPow2(size_t v)
{
	size_t p = 1;
	while (p < v)
		p <<= 1;
	return p;
}

// Spectrum of the analytic version of a real template, zero padded to n.
std::vector<std::complex<float>> analyticSpectrum(const Fft &fft, const std::vector<float> &tpl, double &energy)
{
	const size_t n = fft.size();
	std::vector<std::complex<float>> spec(n);
	energy = 0.0;
	for (size_t i = 0; i < tpl.size(); i++) {
		spec[i] = tpl[i];
		energy += (double)tpl[i] * tpl[i];
	}
	fft.transform(spec, false);
	for (size_t k = 1; k < n / 2; k++)
		spec[k] *= 2.0f;
	for (size_t k = n / 2 + 1; k < n; k++)
		spec[k] = 0.0f;
	// conj() here so that multiplying gives the cross-correlation directly
	for (auto &v : spec)
		v = std::conj(v);
	return spec;
}

} // namespace

ChirpDetector::ChirpDetector(double sampleRate)
	: sampleRate_(sampleRate),
	  tplLen_(spec::makeChirp(sampleRate, false).size()),
	  fftSize_(nextPow2((size_t)(tplLen_ * 2.5))),
	  hop_(fftSize_ - tplLen_ + 1),
	  fft_(fftSize_)
{
	tplSpecUp_ = analyticSpectrum(fft_, spec::makeChirp(sampleRate, false), tplEnergyUp_);
	tplSpecDown_ = analyticSpectrum(fft_, spec::makeChirp(sampleRate, true), tplEnergyDown_);
	work_.resize(fftSize_);
	workUp_.resize(fftSize_);
	workDown_.resize(fftSize_);
}

void ChirpDetector::reset()
{
	buffer_.clear();
	segments_.clear();
	bufStart_ = totalSamples_;
	nextBlockStart_ = totalSamples_;
	expectedNextTs_ = 0;
	inCandidate_ = false;
	candidate_.clear();
	prevPoint_ = {-1, 0, 0, 0};
}

uint64_t ChirpDetector::indexToNs(double index) const
{
	const Segment *seg = nullptr;
	for (auto it = segments_.rbegin(); it != segments_.rend(); ++it) {
		if ((double)it->startIndex <= index) {
			seg = &*it;
			break;
		}
	}
	if (!seg && !segments_.empty())
		seg = &segments_.front();
	if (!seg)
		return 0;
	double offs = (index - (double)seg->startIndex) * 1e9 / sampleRate_;
	return (uint64_t)((double)seg->timestampNs + offs);
}

void ChirpDetector::push(const float *samples, size_t count, uint64_t timestampNs)
{
	if (!count)
		return;

	if (expectedNextTs_ != 0) {
		double diff = (double)timestampNs - (double)expectedNextTs_;
		if (std::fabs(diff) > kResetGapNs)
			reset();
		else if (std::fabs(diff) > kSmoothingThresholdNs)
			segments_.push_back({totalSamples_, timestampNs});
	}
	if (segments_.empty())
		segments_.push_back({totalSamples_, timestampNs});

	buffer_.insert(buffer_.end(), samples, samples + count);
	totalSamples_ += (int64_t)count;
	const Segment &last = segments_.back();
	expectedNextTs_ = last.timestampNs + (uint64_t)((double)(totalSamples_ - last.startIndex) * 1e9 / sampleRate_);

	// level metering in 10 ms blocks
	const size_t levelBlock = (size_t)(sampleRate_ / 100.0);
	for (size_t i = 0; i < count; i++) {
		levelAcc_ += (double)samples[i] * samples[i];
		if (++levelCount_ >= levelBlock) {
			double rms = std::sqrt(levelAcc_ / (double)levelCount_);
			levelPeak_ = std::max(levelPeak_, rms);
			levelAcc_ = 0.0;
			levelCount_ = 0;
			if (++levelBlocks_ >= 100) {
				recentLevelDb_ = levelPeak_ > 1e-7 ? 20.0 * std::log10(levelPeak_) : -140.0;
				levelPeak_ = 0.0;
				levelBlocks_ = 0;
			}
		}
	}

	while (totalSamples_ >= nextBlockStart_ + (int64_t)fftSize_)
		processBlock();

	// keep only what is still needed
	int64_t keepFrom = nextBlockStart_;
	if (keepFrom > bufStart_) {
		size_t drop = (size_t)(keepFrom - bufStart_);
		buffer_.erase(buffer_.begin(), buffer_.begin() + (ptrdiff_t)drop);
		bufStart_ = keepFrom;
	}
	int64_t oldestNeeded = std::min(bufStart_, inCandidate_ && !candidate_.empty() ? candidate_.front().index : bufStart_);
	while (segments_.size() > 1 && segments_[1].startIndex <= oldestNeeded - (int64_t)sampleRate_)
		segments_.pop_front();
}

void ChirpDetector::processBlock()
{
	const size_t n = fftSize_;
	const size_t offset = (size_t)(nextBlockStart_ - bufStart_);
	const float *x = buffer_.data() + offset;

	for (size_t i = 0; i < n; i++)
		work_[i] = x[i];
	fft_.transform(work_, false);
	for (size_t k = 0; k < n; k++) {
		workUp_[k] = work_[k] * tplSpecUp_[k];
		workDown_[k] = work_[k] * tplSpecDown_[k];
	}
	fft_.transform(workUp_, true);
	fft_.transform(workDown_, true);

	// running energy of the template-length window and of its first half
	const size_t half = tplLen_ / 2;
	double energy = 0.0, energyHalf = 0.0;
	for (size_t i = 0; i < tplLen_; i++) {
		energy += (double)x[i] * x[i];
		if (i < half)
			energyHalf += (double)x[i] * x[i];
	}

	const float invN = 1.0f / (float)n;
	for (size_t l = 0; l < hop_; l++) {
		if (l > 0) {
			const double out = x[l - 1];
			const double in = x[l + tplLen_ - 1];
			const double mid = x[l + half - 1];
			energy += in * in - out * out;
			energyHalf += mid * mid - out * out;
			if (energy < 0.0)
				energy = 0.0;
			if (energyHalf < 0.0)
				energyHalf = 0.0;
		}
		// A real, aligned chirp fills the whole window. In digital silence a window that only
		// overlaps the start or the end of a chirp would otherwise correlate strongly
		// (e.g. the low end of an up-chirp with the low end of the down template).
		const double share = energy > 0.0 ? energyHalf / energy : 0.0;
		const bool balanced = share > kMinHalfEnergyShare && share < 1.0 - kMinHalfEnergyShare;
		float up = 0.0f, down = 0.0f;
		if (energy > 1e-9 && balanced) {
			up = (float)(std::abs(workUp_[l]) * invN / std::sqrt(energy * tplEnergyUp_));
			down = (float)(std::abs(workDown_[l]) * invN / std::sqrt(energy * tplEnergyDown_));
		}
		handleScore(nextBlockStart_ + (int64_t)l, up, down, energy);
	}
	nextBlockStart_ += (int64_t)hop_;
}

void ChirpDetector::handleScore(int64_t index, float up, float down, double energy)
{
	const float s = std::max(up, down);

	// quality metering over ~1 s windows
	bestAcc_ = std::max(bestAcc_, (double)s);
	if (index - bestWindowStart_ >= (int64_t)sampleRate_) {
		recentBest_ = bestAcc_;
		bestAcc_ = 0.0;
		bestWindowStart_ = index;
	}

	ScorePoint pt{index, up, down, energy};
	if (inCandidate_) {
		candidate_.push_back(pt);
		if (index >= candidateEnd_)
			finalizeCandidate();
	} else if (index >= refractoryUntil_ && s >= threshold_) {
		inCandidate_ = true;
		candidateEnd_ = index + (int64_t)(kCandidateWindowMs * sampleRate_ / 1000.0);
		candidate_.clear();
		if (prevPoint_.index == index - 1)
			candidate_.push_back(prevPoint_);
		candidate_.push_back(pt);
	}
	prevPoint_ = pt;
}

void ChirpDetector::finalizeCandidate()
{
	inCandidate_ = false;
	if (candidate_.size() < 3) {
		candidate_.clear();
		return;
	}

	auto score = [](const ScorePoint &p) { return std::max(p.up, p.down); };

	float maxScore = 0.0f;
	for (const auto &p : candidate_)
		maxScore = std::max(maxScore, score(p));

	// earliest local maximum that is reasonably strong => direct sound, not a reflection
	size_t peak = 0;
	bool found = false;
	for (size_t i = 1; i + 1 < candidate_.size(); i++) {
		float s = score(candidate_[i]);
		if (s >= kDirectPathRatio * maxScore && s >= score(candidate_[i - 1]) && s >= score(candidate_[i + 1])) {
			peak = i;
			found = true;
			break;
		}
	}
	if (!found) {
		candidate_.clear();
		return;
	}

	const ScorePoint &p = candidate_[peak];
	const bool marker = p.down > p.up;
	auto val = [&](size_t i) { return marker ? candidate_[i].down : candidate_[i].up; };
	double ym = val(peak - 1), y0 = val(peak), yp = val(peak + 1);
	double denom = ym - 2.0 * y0 + yp;
	double delta = 0.0;
	if (std::fabs(denom) > 1e-12)
		delta = std::clamp(0.5 * (ym - yp) / denom, -0.5, 0.5);

	AudioDetection det;
	det.timeNs = indexToNs((double)p.index + delta);
	det.marker = marker;
	det.score = y0;
	double rms = std::sqrt(p.energy / (double)tplLen_);
	det.levelDb = rms > 1e-7 ? 20.0 * std::log10(rms) : -140.0;

	refractoryUntil_ = p.index + (int64_t)(kRefractoryMs * sampleRate_ / 1000.0);
	candidate_.clear();

	if (callback_)
		callback_(det);
}

} // namespace avs
