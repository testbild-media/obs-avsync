#include "video-detector.hpp"
#include "signal-spec.hpp"

#include <algorithm>
#include <cmath>

namespace avs {

namespace {

constexpr int kCellPx = 4;
constexpr float kFoldAlpha = 0.35f;
constexpr int kRoiUpdateFrames = 15;
constexpr double kRoiKeepJaccard = 0.6;
constexpr double kHistoryMs = 3000.0;
constexpr double kBaselineWindowMs = 2500.0;
constexpr double kPreRollMs = 300.0;
constexpr double kMaxPulseMs = 900.0;
constexpr double kEdgeFraction = 0.05;
constexpr double kMinObservePeriods = 2.5;
constexpr double kMinShapeCorr = 0.7;

double percentile(std::vector<double> v, double p)
{
	if (v.empty())
		return 0.0;
	size_t k = (size_t)std::clamp(p * (double)(v.size() - 1), 0.0, (double)(v.size() - 1));
	std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
	return v[k];
}

} // namespace

FlashDetector::FlashDetector() {}

void FlashDetector::setConfig(const FlashDetectorConfig &cfg)
{
	bool roiChanged = cfg.autoRoi != cfg_.autoRoi || cfg.roiX != cfg_.roiX || cfg.roiY != cfg_.roiY ||
			  cfg.roiW != cfg_.roiW || cfg.roiH != cfg_.roiH || cfg.periodMs != cfg_.periodMs;
	cfg_ = cfg;
	if (roiChanged) {
		std::fill(foldHas_.begin(), foldHas_.end(), 0);
		foldStartMs_ = 0.0;
		roi_.assign(roi_.size(), 0);
		locked_ = false;
		framesSinceRoi_ = kRoiUpdateFrames; // re-evaluate immediately
		resetPulseState();
	}
}

void FlashDetector::reset()
{
	gridW_ = gridH_ = 0;
	cells_.clear();
	prevCells_.clear();
	havePrev_ = false;
	fold_.clear();
	foldHas_.clear();
	roi_.clear();
	locked_ = false;
	resetPulseState();
	status_ = FlashDetectorStatus();
}

void FlashDetector::resetPulseState()
{
	history_.clear();
	inPulse_ = false;
	pulse_.clear();
	pulsePeak_ = 0.0;
	belowCount_ = 0;
	baseline_ = 0.0;
	noise_ = 0.0;
	ampRef_ = 0.0;
	haveLast_ = false;
	prevAbove_ = true; // wait for the signal to drop below the threshold first
	sigmaHistory_.clear();
}

void FlashDetector::resizeGrid(int gw, int gh)
{
	gridW_ = gw;
	gridH_ = gh;
	size_t n = (size_t)gw * (size_t)gh;
	cells_.assign(n, 0.0f);
	prevCells_.assign(n, 0.0f);
	havePrev_ = false;
	fold_.assign(n * kBins, 0.0f);
	foldHas_.assign(n * kBins, 0);
	foldStartMs_ = 0.0;
	roi_.assign(n, 0);
	locked_ = false;
	framesSinceRoi_ = kRoiUpdateFrames;
	resetPulseState();
}

void FlashDetector::pushFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t linesize, uint64_t timeNs)
{
	if (!rgba || width < kCellPx || height < kCellPx)
		return;

	int gw = (int)(width / kCellPx);
	int gh = (int)(height / kCellPx);
	if (gw != gridW_ || gh != gridH_ || width != srcW_ || height != srcH_) {
		srcW_ = width;
		srcH_ = height;
		resizeGrid(gw, gh);
	}

	// luma per cell (BT.709 weights, 8 bit)
	for (int cy = 0; cy < gh; cy++) {
		for (int cx = 0; cx < gw; cx++) {
			uint32_t sum = 0;
			for (int y = 0; y < kCellPx; y++) {
				const uint8_t *row = rgba + (size_t)(cy * kCellPx + y) * linesize + (size_t)cx * kCellPx * 4;
				for (int x = 0; x < kCellPx; x++) {
					const uint8_t *p = row + x * 4;
					sum += 54u * p[0] + 183u * p[1] + 19u * p[2];
				}
			}
			cells_[(size_t)cy * gw + cx] = (float)sum / (256.0f * kCellPx * kCellPx);
		}
	}

	// Manual ROI: fixed cell mask
	if (!cfg_.autoRoi && !locked_) {
		for (int cy = 0; cy < gh; cy++) {
			for (int cx = 0; cx < gw; cx++) {
				double x = (cx + 0.5) / gw, y = (cy + 0.5) / gh;
				bool in = x >= cfg_.roiX && x <= cfg_.roiX + cfg_.roiW && y >= cfg_.roiY &&
					  y <= cfg_.roiY + cfg_.roiH;
				roi_[(size_t)cy * gw + cx] = in ? 1 : 0;
			}
		}
		locked_ = std::any_of(roi_.begin(), roi_.end(), [](uint8_t v) { return v != 0; });
	}

	const double tMs = (double)timeNs / 1e6;

	// Fold profile + ROI selection see every frame (a repeated frame still shows what is on
	// screen at that time). Only the measurement signal below is de-duplicated.
	if (cfg_.autoRoi) {
		if (foldStartMs_ <= 0.0)
			foldStartMs_ = tMs;
		const double period = cfg_.periodMs > 0 ? cfg_.periodMs : spec::kDefaultPeriodMs;
		double phase = std::fmod(tMs, period);
		if (phase < 0)
			phase += period;
		const int bin = std::min(kBins - 1, (int)(phase / period * kBins));
		for (size_t i = 0; i < cells_.size(); i++) {
			const size_t k = i * kBins + (size_t)bin;
			if (foldHas_[k])
				fold_[k] += kFoldAlpha * (cells_[i] - fold_[k]);
			else {
				fold_[k] = cells_[i];
				foldHas_[k] = 1;
			}
		}
		lastFoldMs_ = tMs;
		if (++framesSinceRoi_ >= kRoiUpdateFrames) {
			framesSinceRoi_ = 0;
			updateRoi();
		}
	}

	// Duplicate detection: OBS renders e.g. a 30 fps camera into a 60 fps output, so
	// most frames are repeated. Only the first appearance of a camera frame counts.
	bool duplicate = false;
	if (havePrev_) {
		duplicate = true;
		for (size_t i = 0; i < cells_.size(); i++) {
			if ((!locked_ || roi_[i]) && cells_[i] != prevCells_[i]) {
				duplicate = false;
				break;
			}
		}
	}
	std::swap(cells_, prevCells_);
	havePrev_ = true;
	if (duplicate)
		return;
	const std::vector<float> &cur = prevCells_;

	if (lastFrameMs_ > 0.0) {
		const double dt = tMs - lastFrameMs_;
		if (dt > 0.0 && dt < 500.0)
			frameIntervalMs_ = frameIntervalMs_ > 0.0 ? frameIntervalMs_ + 0.05 * (dt - frameIntervalMs_) : dt;
	}
	lastFrameMs_ = tMs;

	if (!locked_) {
		status_.locked = false;
		return;
	}

	double sum = 0.0;
	int n = 0;
	for (size_t i = 0; i < cur.size(); i++) {
		if (roi_[i]) {
			sum += cur[i];
			n++;
		}
	}
	if (n > 0)
		pushSignal(sum / n, timeNs);
}

void FlashDetector::updateRoi()
{
	const size_t n = (size_t)gridW_ * (size_t)gridH_;
	const double period = cfg_.periodMs > 0 ? cfg_.periodMs : spec::kDefaultPeriodMs;
	const double binMs = period / kBins;

	// Score = pulse height x similarity to a Hann pulse. The similarity check rejects
	// other periodic content, e.g. the seconds digit of a clock that changes once per second.
	std::vector<float> score(n, 0.0f);
	std::vector<float> vals;
	vals.reserve(kBins);
	float best = 0.0f;
	// see a few pulses before deciding (otherwise e.g. clock digits win the first second)
	const bool enoughData = lastFoldMs_ - foldStartMs_ >= kMinObservePeriods * period;
	for (size_t i = 0; i < n; i++) {
		vals.clear();
		int peakBin = -1;
		float mx = -1e9f;
		for (int b = 0; b < kBins; b++) {
			const size_t k = i * kBins + (size_t)b;
			if (!foldHas_[k])
				continue;
			vals.push_back(fold_[k]);
			if (fold_[k] > mx) {
				mx = fold_[k];
				peakBin = b;
			}
		}
		if (vals.size() < (size_t)kBins / 2)
			continue;
		std::nth_element(vals.begin(), vals.begin() + (ptrdiff_t)(vals.size() / 2), vals.end());
		const float med = vals[vals.size() / 2];
		const float height = mx - med;
		if (height < cfg_.minContrast * 0.5f)
			continue;

		double spt = 0, spp = 0, stt = 0;
		for (int b = 0; b < kBins; b++) {
			const size_t k = i * kBins + (size_t)b;
			if (!foldHas_[k])
				continue;
			double d = (double)(b - peakBin) * binMs;
			d -= period * std::round(d / period); // circular distance
			const double t = spec::hann(d, spec::kPulseWidthMs);
			const double p = fold_[k] - med;
			spt += p * t;
			spp += p * p;
			stt += t * t;
		}
		const double corr = (spp > 0 && stt > 0) ? spt / std::sqrt(spp * stt) : 0.0;
		if (corr < kMinShapeCorr)
			continue;
		score[i] = (float)(height * corr);
		best = std::max(best, score[i]);
	}

	std::vector<uint8_t> sel(n, 0);
	bool any = false;
	if (!enoughData && !locked_)
		return;
	if (best >= cfg_.minContrast) {
		const float thr = std::max(0.5f * best, (float)cfg_.minContrast * 0.75f);
		std::vector<uint8_t> cand(n, 0);
		for (size_t i = 0; i < n; i++)
			cand[i] = score[i] >= thr ? 1 : 0;

		// keep the strongest connected area (the phone screen)
		std::vector<int> label(n, -1);
		int bestLabel = -1;
		double bestSum = 0.0;
		std::vector<size_t> stack;
		int next = 0;
		for (size_t i = 0; i < n; i++) {
			if (!cand[i] || label[i] >= 0)
				continue;
			double sum = 0.0;
			stack.push_back(i);
			label[i] = next;
			while (!stack.empty()) {
				const size_t c = stack.back();
				stack.pop_back();
				sum += score[c];
				const int cx = (int)(c % (size_t)gridW_), cy = (int)(c / (size_t)gridW_);
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = cx + dx, ny = cy + dy;
						if (nx < 0 || ny < 0 || nx >= gridW_ || ny >= gridH_)
							continue;
						const size_t nb = (size_t)ny * (size_t)gridW_ + (size_t)nx;
						if (cand[nb] && label[nb] < 0) {
							label[nb] = next;
							stack.push_back(nb);
						}
					}
			}
			if (sum > bestSum) {
				bestSum = sum;
				bestLabel = next;
			}
			next++;
		}
		for (size_t i = 0; i < n; i++) {
			if (label[i] == bestLabel && bestLabel >= 0) {
				sel[i] = 1;
				any = true;
			}
		}
	}

	if (!any) {
		// Keep an existing lock for a while; the fold memory fades slowly anyway.
		if (locked_) {
			bool stillFlashing = false;
			for (size_t i = 0; i < n; i++)
				if (roi_[i] && score[i] >= cfg_.minContrast * 0.5f)
					stillFlashing = true;
			if (stillFlashing)
				return;
		}
		if (locked_)
			resetPulseState();
		roi_.assign(n, 0);
		locked_ = false;
		status_.locked = false;
		return;
	}

	size_t inter = 0, uni = 0;
	for (size_t i = 0; i < n; i++) {
		inter += (sel[i] && roi_[i]) ? 1 : 0;
		uni += (sel[i] || roi_[i]) ? 1 : 0;
	}
	const double jaccard = uni ? (double)inter / (double)uni : 0.0;
	if (locked_ && jaccard >= kRoiKeepJaccard)
		return;

	roi_ = sel;
	locked_ = true;
	status_.roiChanges++;
	resetPulseState();
	// The next pulse may already be under way and would be measured incompletely.
	ignorePulsesBeforeMs_ = lastFrameMs_ + kPreRollMs;
}

void FlashDetector::pushSignal(double value, uint64_t timeNs)
{
	const double tMs = (double)timeNs / 1e6;
	history_.push_back({tMs, value});
	while (!history_.empty() && history_.front().tMs < tMs - kHistoryMs)
		history_.pop_front();

	// baseline = low percentile of recent values (pulses cover at most ~50 % of the time)
	std::vector<double> recent;
	recent.reserve(history_.size());
	for (const auto &s : history_)
		if (s.tMs >= tMs - kBaselineWindowMs)
			recent.push_back(s.v);
	baseline_ = percentile(recent, 0.2);
	if (ampRef_ <= 0.0) {
		double hi = percentile(recent, 0.95);
		ampRef_ = std::max(hi - baseline_, cfg_.minContrast);
	}

	if (haveLast_ && !inPulse_) {
		double d = std::fabs(value - lastValue_);
		noise_ = noise_ > 0.0 ? noise_ + 0.05 * (d - noise_) : d;
	}
	lastValue_ = value;
	haveLast_ = true;

	const double v = value - baseline_;
	const double onThr = std::max({0.3 * ampRef_, 4.0 * noise_, 0.5 * cfg_.minContrast});

	if (!inPulse_) {
		// only a rising edge starts a pulse (never join one half-way through)
		const bool rising = v > onThr && !prevAbove_;
		prevAbove_ = v > onThr;
		if (rising && history_.size() > 5 && tMs >= ignorePulsesBeforeMs_) {
			inPulse_ = true;
			pulse_.clear();
			for (const auto &s : history_)
				if (s.tMs >= tMs - kPreRollMs && s.tMs < tMs)
					pulse_.push_back(s);
			pulse_.push_back({tMs, value});
			pulsePeak_ = v;
			pulseStartMs_ = tMs;
			belowCount_ = 0;
		}
	} else {
		pulse_.push_back({tMs, value});
		pulsePeak_ = std::max(pulsePeak_, v);
		if (v < 0.08 * pulsePeak_)
			belowCount_++;
		else
			belowCount_ = 0;
		if (belowCount_ >= 2 || tMs - pulseStartMs_ > kMaxPulseMs)
			finalizePulse();
	}

	status_.locked = locked_ || !cfg_.autoRoi;
	status_.gridW = gridW_;
	status_.gridH = gridH_;
	status_.cellMask = roi_;
	status_.signal = value;
	status_.baseline = baseline_;
	status_.amplitude = ampRef_;
	status_.inPulse = inPulse_;
	status_.frameRate = frameIntervalMs_ > 0 ? 1000.0 / frameIntervalMs_ : 0.0;
	if (gridW_ > 0 && gridH_ > 0) {
		int x0 = gridW_, y0 = gridH_, x1 = -1, y1 = -1;
		for (int y = 0; y < gridH_; y++)
			for (int x = 0; x < gridW_; x++)
				if (roi_[(size_t)y * gridW_ + x]) {
					x0 = std::min(x0, x);
					y0 = std::min(y0, y);
					x1 = std::max(x1, x);
					y1 = std::max(y1, y);
				}
		if (x1 >= 0) {
			status_.bboxX0 = (double)x0 / gridW_;
			status_.bboxY0 = (double)y0 / gridH_;
			status_.bboxX1 = (double)(x1 + 1) / gridW_;
			status_.bboxY1 = (double)(y1 + 1) / gridH_;
		}
	}
}

void FlashDetector::finalizePulse()
{
	inPulse_ = false;
	std::vector<Sample> pts;
	pts.swap(pulse_);
	if (pts.size() < 2)
		return;

	// baseline-corrected values
	size_t peakIdx = 0;
	double peak = -1e9;
	for (size_t i = 0; i < pts.size(); i++) {
		pts[i].v -= baseline_;
		if (pts[i].v > peak) {
			peak = pts[i].v;
			peakIdx = i;
		}
	}
	if (peak < cfg_.minContrast)
		return;

	// contiguous region above the edge threshold, plus one sample on each side
	const double edge = kEdgeFraction * peak;
	size_t lo = peakIdx, hi = peakIdx;
	while (lo > 0 && pts[lo - 1].v >= edge)
		lo--;
	while (hi + 1 < pts.size() && pts[hi + 1].v >= edge)
		hi++;
	if (lo > 0)
		lo--;
	if (hi + 1 < pts.size())
		hi++;

	// piecewise-linear moments (robust against dropped / irregular frames)
	double m0 = 0, m1 = 0, m2 = 0;
	const double tRef = pts[peakIdx].tMs;
	for (size_t i = lo; i < hi; i++) {
		double t0 = pts[i].tMs - tRef, t1 = pts[i + 1].tMs - tRef;
		double v0 = std::max(0.0, pts[i].v), v1 = std::max(0.0, pts[i + 1].v);
		double dt = t1 - t0;
		if (dt <= 0)
			continue;
		m0 += dt * (v0 + v1) / 2.0;
		m1 += dt * (t0 * (2 * v0 + v1) + t1 * (v0 + 2 * v1)) / 6.0;
		m2 += dt * (v0 * (3 * t0 * t0 + 2 * t0 * t1 + t1 * t1) + v1 * (t0 * t0 + 2 * t0 * t1 + 3 * t1 * t1)) /
		      12.0;
	}
	if (m0 <= 0.0)
		return;
	double c = m1 / m0;
	double var = std::max(0.0, m2 / m0 - c * c);

	ampRef_ = ampRef_ > 0 ? ampRef_ + 0.3 * (peak - ampRef_) : peak;

	VideoDetection det;
	det.timeNs = (uint64_t)((tRef + c) * 1e6);
	det.sigmaMs = std::sqrt(var);

	// Marker pulses are 2.5x wider. The absolute width depends on the camera's gamma
	// and exposure, so compare against the typical width of recent (mostly normal) pulses.
	double ref = spec::kNormalSigmaGuessMs;
	if (sigmaHistory_.size() >= 4)
		ref = percentile(std::vector<double>(sigmaHistory_.begin(), sigmaHistory_.end()), 0.3);
	det.marker = det.sigmaMs > spec::kMarkerSigmaRatio * ref;
	sigmaHistory_.push_back(det.sigmaMs);
	while (sigmaHistory_.size() > 15)
		sigmaHistory_.pop_front();
	det.amplitude = peak;
	det.samples = (int)(hi - lo + 1);
	if (callback_)
		callback_(det);
}

} // namespace avs
