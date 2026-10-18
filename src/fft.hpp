// Minimal iterative radix-2 complex FFT (single precision, fixed size).
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace avs {

class Fft {
public:
	explicit Fft(size_t n);

	size_t size() const { return n_; }

	// In-place transform. inverse=true computes the unscaled inverse transform.
	void transform(std::vector<std::complex<float>> &data, bool inverse) const;

private:
	size_t n_;
	std::vector<size_t> rev_;
	std::vector<std::complex<float>> twiddle_;
};

} // namespace avs
