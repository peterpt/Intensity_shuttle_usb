// A fake capture device that sends single-color frames at a given rate.
// Mostly useful for testing themes without actually hooking up capture devices.

#include "bmusb/fake_capture.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#if __SSE2__
#include <immintrin.h>
#endif
#include <chrono>
#include <cstddef>

#include "bmusb/bmusb.h"

#define FRAME_SIZE (8 << 20)  // 8 MB.

// Pure-color inputs: Red, green, blue, white, two shades of gray.
#define NUM_COLORS 6
constexpr uint8_t ys[NUM_COLORS] = { 63, 173, 32, 235, 180, 128 };
constexpr uint8_t cbs[NUM_COLORS] = { 102, 42, 240, 128, 128, 128 };
constexpr uint8_t crs[NUM_COLORS] = { 240, 26, 118, 128, 128, 128 };

using namespace std;
using namespace std::chrono;

namespace bmusb {
namespace {

// We don't bother with multiversioning for this, because SSE2
// is on by default for all 64-bit compiles, which is really
// the target user segment here.

void memset2(uint8_t *s, const uint8_t c[2], size_t n)
{
	size_t i = 0;
#if __SSE2__
	const uint8_t c_expanded[16] = {
		c[0], c[1], c[0], c[1], c[0], c[1], c[0], c[1],
		c[0], c[1], c[0], c[1], c[0], c[1], c[0], c[1]
	};
	__m128i cc = *(__m128i *)c_expanded;
	__m128i *out = (__m128i *)s;

	for ( ; i < (n & ~15); i += 16) {
		_mm_storeu_si128(out++, cc);
		_mm_storeu_si128(out++, cc);
	}

	s = (uint8_t *)out;
#endif
	for ( ; i < n; ++i) {
		*s++ = c[0];
		*s++ = c[1];
	}
}

void memset4(uint8_t *s, const uint8_t c[4], size_t n)
{
	size_t i = 0;
#if __SSE2__
	const uint8_t c_expanded[16] = {
		c[0], c[1], c[2], c[3], c[0], c[1], c[2], c[3],
		c[0], c[1], c[2], c[3], c[0], c[1], c[2], c[3]
	};
	__m128i cc = *(__m128i *)c_expanded;
	__m128i *out = (__m128i *)s;

	for ( ; i < (n & ~7); i += 8) {
		_mm_storeu_si128(out++, cc);
		_mm_storeu_si128(out++, cc);
	}

	s = (uint8_t *)out;
#endif
	for ( ; i < n; ++i) {
		*s++ = c[0];
		*s++ = c[1];
		*s++ = c[2];
		*s++ = c[3];
	}
}

void memset16(uint8_t *s, const uint32_t c[4], size_t n)
{
	size_t i = 0;
#if __SSE2__
	__m128i cc = *(__m128i *)c;
	__m128i *out = (__m128i *)s;

	for ( ; i < (n & ~1); i += 2) {
		_mm_storeu_si128(out++, cc);
		_mm_storeu_si128(out++, cc);
	}

	s = (uint8_t *)out;
#endif
	for ( ; i < n; ++i) {
		memcpy(s, c, 16);
		s += 16;
	}
}

}  // namespace

FakeCapture::FakeCapture(unsigned width, unsigned height, unsigned fps, unsigned audio_sample_frequency, int card_index, bool has_audio)
	: width(width), height(height), fps(fps), audio_sample_frequency(audio_sample_frequency), card_index(card_index)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "Fake card %d", card_index + 1);
	description = buf;

	y = ys[card_index % NUM_COLORS];
	cb = cbs[card_index % NUM_COLORS];
	cr = crs[card_index % NUM_COLORS];

	if (has_audio) {
		audio_ref_level = pow(10.0f, -23.0f / 20.0f) * (1u << 31);  // -23 dBFS (EBU R128 level).

		float freq = 440.0 * pow(2.0, card_index / 12.0);
		sincosf(2 * M_PI * freq / audio_sample_frequency, &audio_sin, &audio_cos);
		audio_real = audio_ref_level;
		audio_imag = 0.0f;
	}
}

FakeCapture::~FakeCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

void FakeCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(65536, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
}

void FakeCapture::start_bm_capture()
{
	producer_thread_should_quit = false;
	producer_thread = thread(&FakeCapture::producer_thread_func, this);
}

void FakeCapture::stop_dequeue_thread()
{
	producer_thread_should_quit = true;
	producer_thread.join();
}
	
std::map<uint32_t, VideoMode> FakeCapture::get_available_video_modes() const
{
	VideoMode mode;

	char buf[256];
	snprintf(buf, sizeof(buf), "%ux%u", width, height);
	mode.name = buf;
	
	mode.autodetect = false;
	mode.width = width;
	mode.height = height;
	mode.frame_rate_num = fps;
	mode.frame_rate_den = 1;
	mode.interlaced = false;

	return {{ 0, mode }};
}

std::map<uint32_t, std::string> FakeCapture::get_available_video_inputs() const
{
	return {{ 0, "Fake video input (single color)" }};
}

std::map<uint32_t, std::string> FakeCapture::get_available_audio_inputs() const
{
	return {{ 0, "Fake audio input (silence)" }};
}

void FakeCapture::set_video_mode(uint32_t video_mode_id)
{
	assert(video_mode_id == 0);
}

void FakeCapture::set_video_input(uint32_t video_input_id)
{
	assert(video_input_id == 0);
}

void FakeCapture::set_audio_input(uint32_t audio_input_id)
{
	assert(audio_input_id == 0);
}

namespace {

void add_time(double t, timespec *ts)
{
	ts->tv_nsec += lrint(t * 1e9);
	ts->tv_sec += ts->tv_nsec / 1000000000;
	ts->tv_nsec %= 1000000000;
}

bool timespec_less_than(const timespec &a, const timespec &b)
{
	return make_pair(a.tv_sec, a.tv_nsec) < make_pair(b.tv_sec, b.tv_nsec);
}

void fill_color_noninterleaved(uint8_t *dst, uint8_t y, uint8_t cb, uint8_t cr, const VideoFormat &video_format, bool ten_bit)
{
	if (ten_bit) {
		// Just use the 8-bit-values shifted left by 2.
		// It's not 100% correct, but it's close enough.
		uint32_t pix[4];
		pix[0] = (cb << 2) | (y  << 12) | (cr << 22);
		pix[1] = (y  << 2) | (cb << 12) | ( y << 22);
		pix[2] = (cr << 2) | (y  << 12) | (cb << 22);
		pix[3] = (y  << 2) | (cr << 12) | ( y << 22);
		memset16(dst, pix, video_format.stride * video_format.height / sizeof(pix));
	} else {
		uint8_t ycbcr[] = { cb, y, cr, y };
		memset4(dst, ycbcr, video_format.width * video_format.height / 2);
	}
}

}  // namespace

void FakeCapture::producer_thread_func()
{
	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "FakeCapture_%d", card_index);
	pthread_setname_np(pthread_self(), thread_name);

	uint16_t timecode = 0;

	if (has_dequeue_callbacks) {
		dequeue_init_callback();
	}

	timespec next_frame;
	clock_gettime(CLOCK_MONOTONIC, &next_frame);
	add_time(1.0 / fps, &next_frame);

	while (!producer_thread_should_quit) {
		timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);

		if (timespec_less_than(now, next_frame)) {
			// Wait until the next frame.
			if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                            &next_frame, nullptr) == -1) {
				if (errno == EINTR) continue;  // Re-check the flag and then sleep again.
				perror("clock_nanosleep");
				exit(1);
			}
		} else {
			// We've seemingly missed a frame. If we're more than one second behind,
			// reset the timer; otherwise, just keep going.
			timespec limit = next_frame;
			++limit.tv_sec;
			if (!timespec_less_than(now, limit)) {
				fprintf(stderr, "More than one second of missed fake frames; resetting clock.\n");
				next_frame = now;
			}
		}
		steady_clock::time_point timestamp = steady_clock::now();

		// Figure out when the next frame is to be, then compute the current one.
		add_time(1.0 / fps, &next_frame);

		VideoFormat video_format;
		video_format.width = width;
		video_format.height = height;
		if (current_pixel_format == PixelFormat_10BitYCbCr) {
			video_format.stride = (width + 5) / 6 * 4 * sizeof(uint32_t);
		} else {
			video_format.stride = width * 2;
		}
		video_format.frame_rate_nom = fps;
		video_format.frame_rate_den = 1;
		video_format.has_signal = true;
		video_format.is_connected = false;

		FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
		if (video_frame.data != nullptr) {
			assert(video_frame.size >= width * height * 2);
			if (video_frame.interleaved) {
				assert(current_pixel_format == PixelFormat_8BitYCbCr);
				uint8_t cbcr[] = { cb, cr };
				memset2(video_frame.data, cbcr, width * height / 2);
				memset(video_frame.data2, y, width * height);
			} else {
				fill_color_noninterleaved(video_frame.data, y, cb, cr, video_format, current_pixel_format == PixelFormat_10BitYCbCr);
			}
			if (video_frame.data_copy != nullptr) {
				fill_color_noninterleaved(video_frame.data_copy, y, cb, cr, video_format, current_pixel_format == PixelFormat_10BitYCbCr);
			}
			video_frame.len = video_format.stride * height;
			video_frame.received_timestamp = timestamp;
		}

		AudioFormat audio_format;
		audio_format.bits_per_sample = 32;
		audio_format.num_channels = 8;

		FrameAllocator::Frame audio_frame = audio_frame_allocator->alloc_frame();
		if (audio_frame.data != nullptr) {
			const unsigned num_stereo_samples = audio_sample_frequency / fps;
			assert(audio_frame.size >= audio_format.num_channels * sizeof(int32_t) * num_stereo_samples);
			audio_frame.len = audio_format.num_channels * sizeof(int32_t) * num_stereo_samples;
			audio_frame.received_timestamp = timestamp;

			if (audio_sin == 0.0f) {
				// Silence.
				memset(audio_frame.data, 0, audio_frame.len);
			} else {
				make_tone((int32_t *)audio_frame.data, num_stereo_samples, audio_format.num_channels);
			}
		}

		frame_callback(timecode++,
			       video_frame, 0, video_format,
			       audio_frame, 0, audio_format);
	}
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

void FakeCapture::make_tone(int32_t *out, unsigned num_stereo_samples, unsigned num_channels)
{
	int32_t *ptr = out;
	float r = audio_real, i = audio_imag;
	for (unsigned sample_num = 0; sample_num < num_stereo_samples; ++sample_num) {
		int32_t s = lrintf(r);
		for (unsigned i = 0; i < num_channels; ++i) {
			*ptr++ = s;
		}

		// Rotate the phaser by one sample.
		float new_r = r * audio_cos - i * audio_sin;
		float new_i = r * audio_sin + i * audio_cos;
		r = new_r;
		i = new_i;
	}

	// Periodically renormalize to counteract precision issues.
	double corr = audio_ref_level / hypot(r, i);
	audio_real = r * corr;
	audio_imag = i * corr;
}

}  // namespace bmusb
