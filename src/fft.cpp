#include "fft.hpp"

#include <cmath>
#include <stdexcept>

namespace avs {

Fft::Fft(size_t n) : n_(n)
{
	if (n < 2 || (n & (n - 1)) != 0)
		throw std::invalid_argument("FFT size must be a power of two");

	size_t bits = 0;
	while (((size_t)1 << bits) < n)
		bits++;

	rev_.resize(n);
	for (size_t i = 0; i < n; i++) {
		size_t r = 0;
		for (size_t b = 0; b < bits; b++)
			if (i & ((size_t)1 << b))
				r |= (size_t)1 << (bits - 1 - b);
		rev_[i] = r;
	}

	twiddle_.resize(n / 2);
	const double pi = 3.14159265358979323846;
	for (size_t i = 0; i < n / 2; i++) {
		double a = -2.0 * pi * (double)i / (double)n;
		twiddle_[i] = {(float)std::cos(a), (float)std::sin(a)};
	}
}

void Fft::transform(std::vector<std::complex<float>> &data, bool inverse) const
{
	if (data.size() != n_)
		throw std::invalid_argument("FFT buffer size mismatch");

	for (size_t i = 0; i < n_; i++)
		if (i < rev_[i])
			std::swap(data[i], data[rev_[i]]);

	for (size_t len = 2; len <= n_; len <<= 1) {
		size_t half = len >> 1;
		size_t step = n_ / len;
		for (size_t start = 0; start < n_; start += len) {
			for (size_t k = 0; k < half; k++) {
				std::complex<float> w = twiddle_[k * step];
				if (inverse)
					w = std::conj(w);
				std::complex<float> &a = data[start + k];
				std::complex<float> &b = data[start + k + half];
				std::complex<float> t = w * b;
				b = a - t;
				a = a + t;
			}
		}
	}
}

} // namespace avs
