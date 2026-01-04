// Intensity Shuttle USB3 capture driver, v0.7.8
// Can download 8-bit and 10-bit UYVY/v210-ish frames from HDMI, quite stable
// (can do captures for hours at a time with no drops), except during startup
// 576p60/720p60/1080i60 works, 1080p60 does not work (firmware limitation)
// Audio comes out as 8-channel 24-bit raw audio.

//#if (defined(__i386__) || defined(__x86_64__)) && defined(__GNUC__)
//#define HAS_MULTIVERSIONING 0
//#endif

    
// --- FIX 1: DISABLE AVX OPTIMIZATIONS ---
#include <assert.h>
#include <errno.h>
#include <libusb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bmusb/bmusb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stack>
#include <string>
#include <thread>

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

#define USB_VENDOR_BLACKMAGIC 0x1edb
#define MIN_WIDTH 640
#define HEADER_SIZE 44
#define AUDIO_HEADER_SIZE 4

#define FRAME_SIZE (8 << 20)  // 8 MB.
#define USB_VIDEO_TRANSFER_SIZE (128 << 10)  // 128 kB.

namespace bmusb {

card_connected_callback_t BMUSBCapture::card_connected_callback = nullptr;
bool BMUSBCapture::hotplug_existing_devices = false;

namespace {

FILE *audiofp;

thread usb_thread;
atomic<bool> should_quit;

int v210_stride(int width)
{
	return (width + 5) / 6 * 4 * sizeof(uint32_t);
}

int find_xfer_size_for_width(PixelFormat pixel_format, int width)
{
    // CRITICAL FIX:
    // The driver defaults to 'assumed_frame_width = 1280' on startup.
    // If the actual signal is 1080p, the hardware sends large bursts.
    // If we only allocate 15360 (optimized for 720p), 1080p overflows immediately.
    // We must return 32KB (Max safe size) for ANY HD resolution (720p or 1080p).
    // This allows the first 1080p frame to arrive successfully so the driver 
    // can detect the resolution and lock on.
    
    if (width >= 1280) {
        return 32768; // 128KB buffer / 32KB = 4 packets per transfer.
    }

    // SD Logic (480i/576i)
    int stride;
    if (pixel_format == PixelFormat_10BitYCbCr) {
        stride = v210_stride(width);
    } else {
        stride = width * sizeof(uint16_t);
    }

    int size = stride * 6;
    if (size % 1024 != 0) {
        size &= ~1023;
        size += 1024;
    }
    
    if (size > 32768) return 32768;

    return size;
}

void change_xfer_size_for_width(PixelFormat pixel_format, int width, libusb_transfer *xfr)
{
	assert(width >= MIN_WIDTH);
	size_t size = find_xfer_size_for_width(pixel_format, width);
	int num_iso_pack = xfr->length / size;

    // --- DEBUG ADDITION ---
    // Only print if we are actually changing the size to avoid spamming
	if (num_iso_pack != xfr->num_iso_packets || size != xfr->iso_packet_desc[0].length) {
        printf("[DEBUG] Resizing Transfer: Width=%d, CalcSize=%ld, NumPackets=%d\n", 
               width, size, num_iso_pack);
        
		xfr->num_iso_packets = num_iso_pack;
		libusb_set_iso_packet_lengths(xfr, size);
	}
}

struct VideoFormatEntry {
	uint16_t normalized_video_format;
	unsigned width, height, second_field_start;
	unsigned extra_lines_top, extra_lines_bottom;
	unsigned frame_rate_nom, frame_rate_den;
	bool interlaced;
};

bool decode_video_format(uint16_t video_format, VideoFormat *decoded_video_format)
{
	decoded_video_format->id = video_format;
	decoded_video_format->interlaced = false;
	decoded_video_format->extra_lines_top = decoded_video_format->extra_lines_bottom = decoded_video_format->second_field_start = 0;

	if (video_format == 0x0800) {
		decoded_video_format->width = 720;
		decoded_video_format->height = 525;
		decoded_video_format->stride = 720 * 2;
		decoded_video_format->extra_lines_top = 0;
		decoded_video_format->extra_lines_bottom = 0;
		decoded_video_format->frame_rate_nom = 3013;
		decoded_video_format->frame_rate_den = 100;
		decoded_video_format->has_signal = false;
		return true;
	}
	if ((video_format & 0xe000) != 0xe000) {
		printf("Video format 0x%04x does not appear to be a video format. Assuming 60 Hz.\n", video_format);
		decoded_video_format->width = 0;
		decoded_video_format->height = 0;
		decoded_video_format->stride = 0;
		decoded_video_format->extra_lines_top = 0;
		decoded_video_format->extra_lines_bottom = 0;
		decoded_video_format->frame_rate_nom = 60;
		decoded_video_format->frame_rate_den = 1;
		decoded_video_format->has_signal = false;
		return false;
	}

	decoded_video_format->has_signal = true;

	if ((video_format & ~0x0800) == 0xe101 ||
	    (video_format & ~0x0800) == 0xe1c1 ||
	    (video_format & ~0x0800) == 0xe001) {
		decoded_video_format->width = 720;
		decoded_video_format->height = 480;
		if (video_format & 0x0800) {
			decoded_video_format->stride = 720 * 2;
		} else {
			decoded_video_format->stride = v210_stride(720);
		}
		decoded_video_format->extra_lines_top = 17;
		decoded_video_format->extra_lines_bottom = 28;
		decoded_video_format->frame_rate_nom = 30000;
		decoded_video_format->frame_rate_den = 1001;
		decoded_video_format->second_field_start = 280;
		decoded_video_format->interlaced = true;
		return true;
	}

	if ((video_format & ~0x0800) == 0xe109 ||
	    (video_format & ~0x0800) == 0xe1c9 ||
	    (video_format & ~0x0800) == 0xe009 ||
	    (video_format & ~0x0800) == 0xe3e9 ||
	    (video_format & ~0x0800) == 0xe3e1) {
		decoded_video_format->width = 720;
		decoded_video_format->height = 576;
		if (video_format & 0x0800) {
			decoded_video_format->stride = 720 * 2;
		} else {
			decoded_video_format->stride = v210_stride(720);
		}
		decoded_video_format->extra_lines_top = 22;
		decoded_video_format->extra_lines_bottom = 27;
		decoded_video_format->frame_rate_nom = 25;
		decoded_video_format->frame_rate_den = 1;
		decoded_video_format->second_field_start = 335;
		decoded_video_format->interlaced = true;
		return true;
	}

	uint16_t normalized_video_format = video_format & ~0xe80c;
	constexpr VideoFormatEntry entries[] = {
		{ 0x01f1,  720,  480,   0, 40,  5, 60000, 1001, false },  
		{ 0x0131,  720,  576,   0, 44,  5,    50,    1, false },
		{ 0x0141, 1280,  720,   0, 25,  5,    50,    1, false },  // 720p50 Fix 
		{ 0x0151,  720,  576,   0, 44,  5,    50,    1, false },  
		{ 0x0011,  720,  576,   0, 44,  5,    50,    1, false },  
		{ 0x0143, 1280,  720,   0, 25,  5,    50,    1, false }, 
		{ 0x0161, 1280,  720,   0, 25,  5,    50,    1, false }, 
		{ 0x0103, 1280,  720,   0, 25,  5,    60,    1, false }, 
		{ 0x0125, 1280,  720,   0, 25,  5,    60,    1, false }, 
		{ 0x0121, 1280,  720,   0, 25,  5, 60000, 1001, false }, 
		{ 0x01c3, 1920, 1080,   0, 41,  4,    30,    1, false }, 
		{ 0x0003, 1920, 1080, 583, 20, 25,    30,    1,  true }, 
		{ 0x01e1, 1920, 1080,   0, 41,  4, 30000, 1001, false }, 
		{ 0x0021, 1920, 1080, 583, 20, 25, 30000, 1001,  true }, 
		{ 0x0063, 1920, 1080,   0, 41,  4,    25,    1, false }, 
		{ 0x0043, 1920, 1080, 583, 20, 25,    25,    1,  true }, 
		{ 0x0083, 1920, 1080,   0, 41,  4,    24,    1, false }, 
		{ 0x00a1, 1920, 1080,   0, 41,  4, 24000, 1001, false }, 
	};
	for (const VideoFormatEntry &entry : entries) {
		if (normalized_video_format == entry.normalized_video_format) {
			decoded_video_format->width = entry.width;
			decoded_video_format->height = entry.height;
			if (video_format & 0x0800) {
				decoded_video_format->stride = entry.width * 2;
			} else {
				decoded_video_format->stride = v210_stride(entry.width);
			}
			decoded_video_format->second_field_start = entry.second_field_start;
			decoded_video_format->extra_lines_top = entry.extra_lines_top;
			decoded_video_format->extra_lines_bottom = entry.extra_lines_bottom;
			decoded_video_format->frame_rate_nom = entry.frame_rate_nom;
			decoded_video_format->frame_rate_den = entry.frame_rate_den;
			decoded_video_format->interlaced = entry.interlaced;
			return true;
		}
	}

    // --- MODIFICATION START ---
    // Instead of lying about 720p, we mark it as 2x2.
    // Shim will recognize this as "Unsupported".
    // We print the hex code so you can add it to the table later if it's valid.
    printf("Unsupported video format: 0x%04x\n", video_format);
    
    decoded_video_format->width = 2; 
    decoded_video_format->height = 2;
    decoded_video_format->stride = 4;
    decoded_video_format->frame_rate_nom = 1; 
    decoded_video_format->frame_rate_den = 1;
    
    // Return TRUE so the data is actually passed to the callback/shim
    return true; 
}

int guess_sample_rate(const VideoFormat &video_format, size_t len, int default_rate)
{
	size_t num_samples = len / 3 / 8;
	size_t num_samples_per_second = num_samples * video_format.frame_rate_nom / video_format.frame_rate_den;

	const int candidate_sample_rates[] = { 32000, 44100, 48000 };
	for (int rate : candidate_sample_rates) {
		if (abs(int(num_samples_per_second) - rate) <= 100) {
			return rate;
		}
	}

	// fprintf(stderr, "%ld samples at %d/%d fps (%ld Hz) matches no known sample rate, keeping capture at %d Hz\n",
	//	num_samples, video_format.frame_rate_nom, video_format.frame_rate_den, num_samples_per_second, default_rate);
	return default_rate;
}

}  // namespace

FrameAllocator::~FrameAllocator() {}

MallocFrameAllocator::MallocFrameAllocator(size_t frame_size, size_t num_queued_frames)
	: frame_size(frame_size)
{
	for (size_t i = 0; i < num_queued_frames; ++i) {
		freelist.push(unique_ptr<uint8_t[]>(new uint8_t[frame_size]));
	}
}

FrameAllocator::Frame MallocFrameAllocator::alloc_frame()
{
	Frame vf;
	vf.owner = this;

	unique_lock<mutex> lock(freelist_mutex); 
	if (freelist.empty()) {
		printf("Frame overrun (no more spare frames of size %ld), dropping frame!\n",
			frame_size);
	} else {
		vf.data = freelist.top().release();
		vf.size = frame_size;
		freelist.pop(); 
	}
	return vf;
}

void MallocFrameAllocator::release_frame(Frame frame)
{
	if (frame.overflow > 0) {
		printf("%d bytes overflow after last (malloc) frame\n", int(frame.overflow));
	}
	unique_lock<mutex> lock(freelist_mutex);
	freelist.push(unique_ptr<uint8_t[]>(frame.data));
}

bool uint16_less_than_with_wraparound(uint16_t a, uint16_t b)
{
	if (a == b) {
		return false;
	} else if (a < b) {
		return (b - a < 0x8000);
	} else {
		int wrap_b = 0x10000 + int(b);
		return (wrap_b - a < 0x8000);
	}
}

void BMUSBCapture::queue_frame(uint16_t format, uint16_t timecode, FrameAllocator::Frame frame, deque<QueuedFrame> *q)
{
	unique_lock<mutex> lock(queue_lock);
	if (!q->empty() && !uint16_less_than_with_wraparound(q->back().timecode, timecode)) {
		printf("Blocks going backwards: prev=0x%04x, cur=0x%04x (dropped)\n",
			q->back().timecode, timecode);
		frame.owner->release_frame(frame);
		return;
	}

	QueuedFrame qf;
	qf.format = format;
	qf.timecode = timecode;
	qf.frame = frame;
	q->push_back(move(qf));
	queues_not_empty.notify_one(); 
}

void dump_frame(const char *filename, uint8_t *frame_start, size_t frame_len)
{
	FILE *fp = fopen(filename, "wb");
	if (fwrite(frame_start + HEADER_SIZE, frame_len - HEADER_SIZE, 1, fp) != 1) {
		printf("short write!\n");
	}
	fclose(fp);
}

void dump_audio_block(uint8_t *audio_start, size_t audio_len)
{
	if (audiofp != nullptr) {
		fwrite(audio_start + AUDIO_HEADER_SIZE, 1, audio_len - AUDIO_HEADER_SIZE, audiofp);
	}
}

void BMUSBCapture::dequeue_thread_func()
{
	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "bmusb_dequeue_%d", card_index);
	pthread_setname_np(pthread_self(), thread_name);

	if (has_dequeue_callbacks) {
		dequeue_init_callback();
	}
	size_t last_sample_rate = 48000;
	while (!dequeue_thread_should_quit) {
		unique_lock<mutex> lock(queue_lock);
		queues_not_empty.wait(lock, [this]{ return dequeue_thread_should_quit || (!pending_video_frames.empty() && !pending_audio_frames.empty()); });

		if (dequeue_thread_should_quit) break;

		uint16_t video_timecode = pending_video_frames.front().timecode;
		AudioFormat audio_format;
		audio_format.bits_per_sample = 24;
		audio_format.num_channels = 8;
		audio_format.sample_rate = last_sample_rate;

		// --- MODIFIED: FORCE VIDEO OUTPUT ---
		// We skipped the "if (video < audio)" checks that were dropping frames.
		QueuedFrame video_frame = pending_video_frames.front();
		QueuedFrame audio_frame = pending_audio_frames.front();
		pending_audio_frames.pop_front();
		pending_video_frames.pop_front();
		lock.unlock();

		VideoFormat video_format;
		audio_format.id = audio_frame.format;
		if (decode_video_format(video_frame.format, &video_format)) {
			if (audio_frame.frame.len != 0) {
				audio_format.sample_rate = guess_sample_rate(video_format, audio_frame.frame.len, last_sample_rate);
				last_sample_rate = audio_format.sample_rate;
			}
			frame_callback(video_timecode,
				       video_frame.frame, HEADER_SIZE, video_format,
				       audio_frame.frame, AUDIO_HEADER_SIZE, audio_format);
		} else {
			video_frame_allocator->release_frame(video_frame.frame);
			audio_format.sample_rate = last_sample_rate;
			frame_callback(video_timecode,
			               FrameAllocator::Frame(), 0, video_format,
				       audio_frame.frame, AUDIO_HEADER_SIZE, audio_format);
		}
	}
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

void BMUSBCapture::start_new_frame(const uint8_t *start)
{
	uint16_t format = (start[3] << 8) | start[2];
	uint16_t timecode = (start[1] << 8) | start[0];

	if (current_video_frame.len > 0) {
		current_video_frame.received_timestamp = steady_clock::now();

		if (format == 0x0800) {
			FrameAllocator::Frame fake_audio_frame = audio_frame_allocator->alloc_frame();
			if (fake_audio_frame.data == nullptr) {
				printf("Couldn't allocate fake audio frame, also dropping no-signal video frame.\n");
				current_video_frame.owner->release_frame(current_video_frame);
				current_video_frame = video_frame_allocator->alloc_frame();
				return;
			}
			queue_frame(format, timecode, fake_audio_frame, &pending_audio_frames);
		}
		queue_frame(format, timecode, current_video_frame, &pending_video_frames);

		VideoFormat video_format;
		if (decode_video_format(format, &video_format)) {
			assumed_frame_width = video_format.width;
		}
	}

	current_video_frame = video_frame_allocator->alloc_frame();
}

void BMUSBCapture::start_new_audio_block(const uint8_t *start)
{
	uint16_t format = (start[3] << 8) | start[2];
	uint16_t timecode = (start[1] << 8) | start[0];
	if (current_audio_frame.len > 0) {
		current_audio_frame.received_timestamp = steady_clock::now();
		queue_frame(format, timecode, current_audio_frame, &pending_audio_frames);
	}
	current_audio_frame = audio_frame_allocator->alloc_frame();
}

void memcpy_interleaved(uint8_t *dest1, uint8_t *dest2, const uint8_t *src, size_t n)
{
	assert(n % 2 == 0);
	uint8_t *dptr1 = dest1;
	uint8_t *dptr2 = dest2;

	for (size_t i = 0; i < n; i += 2) {
		*dptr1++ = *src++;
		*dptr2++ = *src++;
	}
}

void add_to_frame(FrameAllocator::Frame *current_frame, const char *frame_type_name, const uint8_t *start, const uint8_t *end)
{
	if (current_frame->data == nullptr ||
	    current_frame->len > current_frame->size ||
	    start == end) {
		return;
	}

	int bytes = end - start;
	if (current_frame->len + bytes > current_frame->size) {
		current_frame->overflow = current_frame->len + bytes - current_frame->size;
		current_frame->len = current_frame->size;
		if (current_frame->overflow > 1048576) {
			printf("%d bytes overflow after last %s frame\n",
				int(current_frame->overflow), frame_type_name);
			current_frame->overflow = 0;
		}
	} else {
		if (current_frame->data_copy != nullptr) {
			memcpy(current_frame->data_copy + current_frame->len, start, bytes);
		}
		if (current_frame->interleaved) {
			uint8_t *data = current_frame->data + current_frame->len / 2;
			uint8_t *data2 = current_frame->data2 + current_frame->len / 2;
			if (current_frame->len % 2 == 1) {
				++data;
				swap(data, data2);
			}
			if (bytes % 2 == 1) {
				*data++ = *start++;
				swap(data, data2);
				++current_frame->len;
				--bytes;
			}
			memcpy_interleaved(data, data2, start, bytes);
			current_frame->len += bytes;
		} else {
			memcpy(current_frame->data + current_frame->len, start, bytes);
			current_frame->len += bytes;
		}
	}
}

const uint8_t *add_to_frame_fastpath(FrameAllocator::Frame *current_frame, const uint8_t *start, const uint8_t *limit, const char sync_char)
{
    // DISABLED AVX2/SSE: Always return start to use the standard slow path.
    return start;
}

void decode_packs(const libusb_transfer *xfr,
                  const char *sync_pattern,
                  int sync_length,
                  FrameAllocator::Frame *current_frame,
                  const char *frame_type_name,
                  function<void(const uint8_t *start)> start_callback)
{
	int offset = 0;
	for (int i = 0; i < xfr->num_iso_packets; i++) {
		const libusb_iso_packet_descriptor *pack = &xfr->iso_packet_desc[i];

		if (pack->status != LIBUSB_TRANSFER_COMPLETED) {
            // --- DEBUG ADDITION ---
            // Print exactly what happened:
            // Status 6 = Overflow (Hardware sent more bytes than 'Length')
            // Actual_Length = How many bytes the hardware TRIED to send
			fprintf(stderr, "[ERROR] Pack %u/%u Status %d | ReqLen: %u | ActLen: %u\n", 
                i, xfr->num_iso_packets, pack->status, pack->length, pack->actual_length);
			continue;
		}

		const uint8_t *start = xfr->buffer + offset;
		const uint8_t *limit = start + pack->actual_length;
		while (start < limit) { 
			start = add_to_frame_fastpath(current_frame, start, limit, sync_pattern[0]);
			if (start == limit) break;
			assert(start < limit);

			const unsigned char* start_next_frame = (const unsigned char *)memmem(start, limit - start, sync_pattern, sync_length);
			if (start_next_frame == nullptr) {
				add_to_frame(current_frame, frame_type_name, start, limit);
				break;
			} else {
				add_to_frame(current_frame, frame_type_name, start, start_next_frame);
				start = start_next_frame + sync_length;  
				start_callback(start);
			}
		}
		offset += pack->length;
	}
}

void BMUSBCapture::cb_xfr(struct libusb_transfer *xfr)
{
	if (xfr->status != LIBUSB_TRANSFER_COMPLETED &&
	    xfr->status != LIBUSB_TRANSFER_NO_DEVICE) {
		fprintf(stderr, "error: transfer status %d\n", xfr->status);
		libusb_free_transfer(xfr);
		exit(3);
	}

	assert(xfr->user_data != nullptr);
	BMUSBCapture *usb = static_cast<BMUSBCapture *>(xfr->user_data);

	if (xfr->status == LIBUSB_TRANSFER_NO_DEVICE) {
		if (!usb->disconnected) {
			fprintf(stderr, "Device went away, stopping transfers.\n");
			usb->disconnected = true;
			if (usb->card_disconnected_callback) {
				usb->card_disconnected_callback();
			}
		}
		return;
	}

	if (xfr->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
		if (xfr->endpoint == 0x84) {
			decode_packs(xfr, "DeckLinkAudioResyncT", 20, &usb->current_audio_frame, "audio", bind(&BMUSBCapture::start_new_audio_block, usb, _1));
		} else {
			decode_packs(xfr, "\x00\x00\xff\xff", 4, &usb->current_video_frame, "video", bind(&BMUSBCapture::start_new_frame, usb, _1));
			change_xfer_size_for_width(usb->current_pixel_format, usb->assumed_frame_width, xfr);
		}
	}
	if (xfr->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
		uint8_t *buf = libusb_control_transfer_get_data(xfr);
		memcpy(usb->register_file + usb->current_register, buf, 4);
		usb->current_register = (usb->current_register + 4) % NUM_BMUSB_REGISTERS;
		if (usb->current_register == 0) {
			printf("register dump:");
			for (int i = 0; i < NUM_BMUSB_REGISTERS; i += 4) {
				printf(" 0x%02x%02x%02x%02x", usb->register_file[i], usb->register_file[i + 1], usb->register_file[i + 2], usb->register_file[i + 3]);
			}
			printf("\n");
		}
		libusb_fill_control_setup(xfr->buffer,
		    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, 214, 0,
			usb->current_register, 4);
	}

	int rc = libusb_submit_transfer(xfr);
	if (rc < 0) {
		fprintf(stderr, "error re-submitting URB: %s\n", libusb_error_name(rc));
		exit(1);
	}
}

int BMUSBCapture::cb_hotplug(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
	if (card_connected_callback != nullptr) {
		libusb_device_descriptor desc;
                if (libusb_get_device_descriptor(dev, &desc) < 0) {
			fprintf(stderr, "Error getting device descriptor for hotplugged device %p, killing hotplug\n", dev);
			libusb_unref_device(dev);
			return 1;
		}

		if ((desc.idVendor == USB_VENDOR_BLACKMAGIC && desc.idProduct == 0xbd3b) ||
		    (desc.idVendor == USB_VENDOR_BLACKMAGIC && desc.idProduct == 0xbd4f)) {
			card_connected_callback(dev); 
			return 0;
		}
	}
	libusb_unref_device(dev);
	return 0;
}

void BMUSBCapture::usb_thread_func()
{
	sched_param param;
	memset(&param, 0, sizeof(param));
	param.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_RR, &param) == -1) {
		printf("couldn't set realtime priority for USB thread: %s\n", strerror(errno));
	}
	pthread_setname_np(pthread_self(), "bmusb_usb_drv");
	while (!should_quit) {
		timeval sec { 1, 0 };
		int rc = libusb_handle_events_timeout(nullptr, &sec);
		if (rc != LIBUSB_SUCCESS)
			break;
	}
}

namespace {

struct USBCardDevice {
	uint16_t product;
	uint8_t bus, port;
	libusb_device *device;
};

const char *get_product_name(uint16_t product)
{
	if (product == 0xbd3b) {
		return "Intensity Shuttle";
	} else if (product == 0xbd4f) {
		return "UltraStudio SDI";
	} else {
		assert(false);
		return nullptr;
	}
}

string get_card_description(int id, uint8_t bus, uint8_t port, uint16_t product)
{
	const char *product_name = get_product_name(product);

	char buf[256];
	snprintf(buf, sizeof(buf), "USB card %d: Bus %03u Device %03u  %s",
		id, bus, port, product_name);
	return buf;
}

vector<USBCardDevice> find_all_cards()
{
	libusb_device **devices;
	ssize_t num_devices = libusb_get_device_list(nullptr, &devices);
	if (num_devices == -1) {
		fprintf(stderr, "Error finding USB devices\n");
		exit(1);
	}
	vector<USBCardDevice> found_cards;
	for (ssize_t i = 0; i < num_devices; ++i) {
		libusb_device_descriptor desc;
                if (libusb_get_device_descriptor(devices[i], &desc) < 0) {
			fprintf(stderr, "Error getting device descriptor for device %d\n", int(i));
			exit(1);
		}

		uint8_t bus = libusb_get_bus_number(devices[i]);
		uint8_t port = libusb_get_port_number(devices[i]);

		if (!(desc.idVendor == USB_VENDOR_BLACKMAGIC && desc.idProduct == 0xbd3b) &&
		    !(desc.idVendor == USB_VENDOR_BLACKMAGIC && desc.idProduct == 0xbd4f)) {
			libusb_unref_device(devices[i]);
			continue;
		}

		found_cards.push_back({ desc.idProduct, bus, port, devices[i] });
	}
	libusb_free_device_list(devices, 0);

	sort(found_cards.begin(), found_cards.end(), [](const USBCardDevice &a, const USBCardDevice &b) {
		if (a.product != b.product)
			return a.product < b.product;
		if (a.bus != b.bus)
			return a.bus < b.bus;
		return a.port < b.port;
	});

	return found_cards;
}

libusb_device_handle *open_card(int card_index, string *description)
{
	vector<USBCardDevice> found_cards = find_all_cards();

	for (size_t i = 0; i < found_cards.size(); ++i) {
		string tmp_description = get_card_description(i, found_cards[i].bus, found_cards[i].port, found_cards[i].product);
		fprintf(stderr, "%s\n", tmp_description.c_str());
		if (i == size_t(card_index)) {
			*description = tmp_description;
		}
	}

	if (size_t(card_index) >= found_cards.size()) {
		fprintf(stderr, "Could not open card %d (only %d found)\n", card_index, int(found_cards.size()));
		exit(1);
	}

	libusb_device_handle *devh;
	int rc = libusb_open(found_cards[card_index].device, &devh);
	if (rc < 0) {
		fprintf(stderr, "Error opening card %d: %s\n", card_index, libusb_error_name(rc));
		exit(1);
	}

	for (size_t i = 0; i < found_cards.size(); ++i) {
		libusb_unref_device(found_cards[i].device);
	}

	return devh;
}

libusb_device_handle *open_card(unsigned card_index, libusb_device *dev, string *description)
{
	uint8_t bus = libusb_get_bus_number(dev);
	uint8_t port = libusb_get_port_number(dev);

	libusb_device_descriptor desc;
	if (libusb_get_device_descriptor(dev, &desc) < 0) {
		fprintf(stderr, "Error getting device descriptor for device %p\n", dev);
		exit(1);
	}

	*description = get_card_description(card_index, bus, port, desc.idProduct);

	libusb_device_handle *devh;
	int rc = libusb_open(dev, &devh);
	if (rc < 0) {
		fprintf(stderr, "Error opening card %p: %s\n", dev, libusb_error_name(rc));
		exit(1);
	}

	return devh;
}

}  // namespace

unsigned BMUSBCapture::num_cards()
{
	int rc = libusb_init(nullptr);
	if (rc < 0) {
		fprintf(stderr, "Error initializing libusb: %s\n", libusb_error_name(rc));
		exit(1);
	}

	vector<USBCardDevice> found_cards = find_all_cards();
	unsigned ret = found_cards.size();
	for (size_t i = 0; i < found_cards.size(); ++i) {
		libusb_unref_device(found_cards[i].device);
	}
	return ret;
}

void BMUSBCapture::set_pixel_format(PixelFormat pixel_format)
{
	current_pixel_format = pixel_format;
	update_capture_mode();
}

void BMUSBCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(65536, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
	dequeue_thread_should_quit = false;
	dequeue_thread = thread(&BMUSBCapture::dequeue_thread_func, this);

	int rc;
	struct libusb_transfer *xfr;

	rc = libusb_init(nullptr);
	if (rc < 0) {
		fprintf(stderr, "Error initializing libusb: %s\n", libusb_error_name(rc));
		exit(1);
	}

	if (dev == nullptr) {
		devh = open_card(card_index, &description);
	} else {
		devh = open_card(card_index, dev, &description);
		libusb_unref_device(dev);
	}
	if (!devh) {
		fprintf(stderr, "Error finding USB device\n");
		exit(1);
	}

	libusb_config_descriptor *config;
	rc = libusb_get_config_descriptor(libusb_get_device(devh), 0, &config);
	if (rc < 0) {
		fprintf(stderr, "Error getting configuration: %s\n", libusb_error_name(rc));
		exit(1);
	}

	rc = libusb_set_configuration(devh, 1);
	if (rc < 0) {
		fprintf(stderr, "Error setting configuration 1: %s\n", libusb_error_name(rc));
		exit(1);
	}

	rc = libusb_claim_interface(devh, 0);
	if (rc < 0) {
		fprintf(stderr, "Error claiming interface 0: %s\n", libusb_error_name(rc));
		exit(1);
	}

	rc = libusb_set_interface_alt_setting(devh, 0, 1);
	if (rc < 0) {
		fprintf(stderr, "Error setting alternate 1: %s\n", libusb_error_name(rc));
		if (rc == LIBUSB_ERROR_NOT_FOUND) {
			fprintf(stderr, "This is usually because the card came up in USB2 mode.\n");
			fprintf(stderr, "In particular, this tends to happen if you boot up with the\n");
			fprintf(stderr, "card plugged in; just unplug and replug it, and it usually works.\n");
		}
		exit(1);
	}
	rc = libusb_set_interface_alt_setting(devh, 0, 2);
	if (rc < 0) {
		fprintf(stderr, "Error setting alternate 2: %s\n", libusb_error_name(rc));
		exit(1);
	}

	update_capture_mode();

	struct ctrl {
		int endpoint;
		int request;
		int index;
		uint32_t data;
	};
	static const ctrl ctrls[] = {
		{ LIBUSB_ENDPOINT_IN,  214, 16, 0 },
		{ LIBUSB_ENDPOINT_IN,  214,  0, 0 },
		{ LIBUSB_ENDPOINT_OUT, 215, 24, 0x73c60001 }, 
		{ LIBUSB_ENDPOINT_IN,  214, 24, 0 }, 
	};

	for (unsigned req = 0; req < sizeof(ctrls) / sizeof(ctrls[0]); ++req) {
		uint32_t flipped = htonl(ctrls[req].data);
		static uint8_t value[4];
		memcpy(value, &flipped, sizeof(flipped));
		int size = sizeof(value);
		rc = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR | ctrls[req].endpoint,
			ctrls[req].request, 0, ctrls[req].index, value, size, 0);
		if (rc < 0) {
			fprintf(stderr, "Error on control %d: %s\n", ctrls[req].index, libusb_error_name(rc));
			exit(1);
		}

		if (ctrls[req].index == 16 && rc == 4) {
			printf("Card firmware version: 0x%02x%02x\n", value[2], value[3]);
		}
	}

	static uint8_t cmdbuf3[LIBUSB_CONTROL_SETUP_SIZE + 4];
	static int completed3 = 0;

	xfr = libusb_alloc_transfer(0);
	libusb_fill_control_setup(cmdbuf3,
	    LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_IN, 214, 0,
		current_register, 4);
	libusb_fill_control_transfer(xfr, devh, cmdbuf3, cb_xfr, &completed3, 0);
	xfr->user_data = this;

	for (int e = 3; e <= 4; ++e) {
		int num_transfers = 6;
		for (int i = 0; i < num_transfers; ++i) {
			size_t buf_size;
			int num_iso_pack, size;
			if (e == 3) {
				size = find_xfer_size_for_width(PixelFormat_8BitYCbCr, MIN_WIDTH);
				num_iso_pack = USB_VIDEO_TRANSFER_SIZE / size;
				buf_size = USB_VIDEO_TRANSFER_SIZE;
			} else {
				size = 0xc0;
				num_iso_pack = 80;
				buf_size = num_iso_pack * size;
			}
			int num_bytes = num_iso_pack * size;
			assert(size_t(num_bytes) <= buf_size);
#if LIBUSB_API_VERSION >= 0x01000105
			uint8_t *buf = libusb_dev_mem_alloc(devh, num_bytes);
#else
			uint8_t *buf = nullptr;
#endif
			if (buf == nullptr) {
				fprintf(stderr, "Failed to allocate persistent DMA memory ");
				fprintf(stderr, "Will go slower, and likely fail due to memory fragmentation after a few hours.\n");
				buf = new uint8_t[num_bytes];
			}

			xfr = libusb_alloc_transfer(num_iso_pack);
			if (!xfr) {
				fprintf(stderr, "oom\n");
				exit(1);
			}

			int ep = LIBUSB_ENDPOINT_IN | e;
			libusb_fill_iso_transfer(xfr, devh, ep, buf, buf_size,
				num_iso_pack, cb_xfr, nullptr, 0);
			libusb_set_iso_packet_lengths(xfr, size);
			xfr->user_data = this;

			if (e == 3) {
				change_xfer_size_for_width(current_pixel_format, assumed_frame_width, xfr);
			}

			iso_xfrs.push_back(xfr);
		}
	}
}

void BMUSBCapture::start_bm_capture()
{
	int i = 0;
	for (libusb_transfer *xfr : iso_xfrs) {
		int rc = libusb_submit_transfer(xfr);
		++i;
		if (rc < 0) {
			fprintf(stderr, "Error submitting iso to endpoint 0x%02x, number %d: %s\n",
				xfr->endpoint, i, libusb_error_name(rc));
			exit(1);
		}
	}
}

void BMUSBCapture::stop_dequeue_thread()
{
	dequeue_thread_should_quit = true;
	queues_not_empty.notify_all();
	dequeue_thread.join();
}

void BMUSBCapture::start_bm_thread()
{
	if (card_connected_callback != nullptr) {
		if (libusb_hotplug_register_callback(
			nullptr, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, hotplug_existing_devices ? LIBUSB_HOTPLUG_ENUMERATE : LIBUSB_HOTPLUG_NO_FLAGS,
			USB_VENDOR_BLACKMAGIC, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
			&BMUSBCapture::cb_hotplug, nullptr, nullptr) < 0) {
			fprintf(stderr, "libusb_hotplug_register_callback() failed\n");
			exit(1);
		}
	}

	should_quit = false;
	usb_thread = thread(&BMUSBCapture::usb_thread_func);
}

void BMUSBCapture::stop_bm_thread()
{
	should_quit = true;
	libusb_interrupt_event_handler(nullptr);
	usb_thread.join();
}

map<uint32_t, VideoMode> BMUSBCapture::get_available_video_modes() const
{
	VideoMode auto_mode;
	auto_mode.name = "Autodetect";
	auto_mode.autodetect = true;
	return {{ 0, auto_mode }};
}

uint32_t BMUSBCapture::get_current_video_mode() const
{
	return 0;  
}

void BMUSBCapture::set_video_mode(uint32_t video_mode_id)
{
	assert(video_mode_id == 0); 
}

std::map<uint32_t, std::string> BMUSBCapture::get_available_video_inputs() const
{
	return {
		{ 0x00000000, "HDMI/SDI" },
		{ 0x02000000, "Component" },
		{ 0x04000000, "Composite" },
		{ 0x06000000, "S-video" }
	};
}

void BMUSBCapture::set_video_input(uint32_t video_input_id)
{
	assert((video_input_id & ~0x06000000) == 0);
	current_video_input = video_input_id;
	update_capture_mode();
}

std::map<uint32_t, std::string> BMUSBCapture::get_available_audio_inputs() const
{
	return {
		{ 0x00000000, "Embedded" },
		{ 0x10000000, "Analog" }
	};
}

void BMUSBCapture::set_audio_input(uint32_t audio_input_id)
{
	assert((audio_input_id & ~0x10000000) == 0);
	current_audio_input = audio_input_id;
	update_capture_mode();
}

void BMUSBCapture::update_capture_mode()
{
	if (devh == nullptr) {
		return;
	}

	uint32_t mode = htonl(0x09000000 | current_video_input | current_audio_input);
	if (current_pixel_format == PixelFormat_8BitYCbCr) {
		mode |= htonl(0x20000000);
	} else {
		assert(current_pixel_format == PixelFormat_10BitYCbCr);
	}

	int rc = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_ENDPOINT_OUT,
		215, 0, 0, (unsigned char *)&mode, sizeof(mode), 0);
	if (rc < 0) {
		fprintf(stderr, "Error on setting mode: %s\n", libusb_error_name(rc));
		exit(1);
	}
}

BMUSBCapture::~BMUSBCapture() {
    // 1. Ensure threads are stopped explicitly (Safety net)
    if (dequeue_thread.joinable()) {
        dequeue_thread_should_quit = true;
        queues_not_empty.notify_all();
        dequeue_thread.join();
    }
    
    if (usb_thread.joinable()) {
        should_quit = true;
        libusb_interrupt_event_handler(nullptr);
        usb_thread.join();
    }

    // 2. Close the device handle FIRST.
    // CRITICAL FIX: We must close the device before freeing the transfers.
    // libusb_close() needs to access the transfer list to clean up internal state.
    // If we free transfers first, libusb_close() crashes accessing freed memory.
    if (devh) {
        libusb_release_interface(devh, 0);
        libusb_close(devh);
        devh = nullptr;
    }

    // 3. NOW it is safe to free the transfers
    for (libusb_transfer *xfr : iso_xfrs) {
        if (xfr) {
            // Note: We don't need to cancel, because the device is already closed.
            // We just free the structure memory.
            libusb_free_transfer(xfr);
        }
    }
    iso_xfrs.clear();
}
}  // namespace bmusb 

     
