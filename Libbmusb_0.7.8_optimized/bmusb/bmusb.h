#ifndef _BMUSB_H
#define _BMUSB_H

#include <libusb.h>
#include <stdint.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stack>
#include <string>
#include <thread>
#include <vector>

namespace bmusb {

class BMUSBCapture;

// An interface for frame allocators; if you do not specify one
// (using set_video_frame_allocator), a default one that pre-allocates
// a freelist of eight frames using new[] will be used. Specifying
// your own can be useful if you have special demands for where you want the
// frame to end up and don't want to spend the extra copy to get it there, for
// instance GPU memory.
class FrameAllocator {
 public:
	struct Frame {
		uint8_t *data = nullptr;
		uint8_t *data2 = nullptr;  // Only if interleaved == true.
		uint8_t *data_copy = nullptr;  // Will get a non-interleaved copy if not nullptr.
		size_t len = 0;  // Number of bytes we actually have.
		size_t size = 0;  // Number of bytes we have room for.
		size_t overflow = 0;
		void *userdata = nullptr;
		FrameAllocator *owner = nullptr;

		// If set to true, every other byte will go to data and to data2.
		// If so, <len> and <size> are still about the number of total bytes
		// so if size == 1024, there's 512 bytes in data and 512 in data2.
		//
		// This doesn't really make any sense if you asked for the
		// 10BitYCbCr pixel format.
		bool interleaved = false;

		// At what point this frame was received. Note that this marks the
		// _end_ of the frame being received, not the beginning.
		// Thus, if you want to measure latency, you'll also need to include
		// the time the frame actually took to transfer (usually 1/fps,
		// ie., the frames are typically transferred in real time).
		std::chrono::steady_clock::time_point received_timestamp =
			std::chrono::steady_clock::time_point::min();
	};

	virtual ~FrameAllocator();

	// Request a video frame. Note that this is called from the
	// USB thread, which runs with realtime priority and is
	// very sensitive to delays. Thus, you should not do anything
	// here that might sleep, including calling malloc().
	// (Taking a mutex is borderline.)
	//
	// The Frame object will be given to the frame callback,
	// which is responsible for releasing the video frame back
	// once it is usable for new frames (ie., it will no longer
	// be read from). You can use the "userdata" pointer for
	// whatever you want to identify this frame if you need to.
	//
	// Returning a Frame with data==nullptr is allowed;
	// if so, the frame in progress will be dropped.
	virtual Frame alloc_frame() = 0;

	// Similar to alloc_frame(), with two additional restrictions:
	//
	//  - The width, height and stride given must be correct
	//    (can not be changed after the call).
	//  - create_frame(), unlike alloc_frame(), is allowed to sleep
	//    (so bmusb will never call it, but in Nageru, other producers
	//    might)
	//
	// These two restrictions are relevant for Nageru, since it means that
	// it can make frame_copy point directly into a VA-API buffer to avoid
	// an extra copy.
	virtual Frame create_frame(size_t width, size_t height, size_t stride)
	{
		return alloc_frame();
	}

	virtual void release_frame(Frame frame) = 0;
};

// Audio is more important than video, and also much cheaper.
// By having many more audio frames available, hopefully if something
// starts to drop, we'll have CPU load go down (from not having to
// process as much video) before we have to drop audio.
#define NUM_QUEUED_VIDEO_FRAMES 128
#define NUM_QUEUED_AUDIO_FRAMES 512

class MallocFrameAllocator : public FrameAllocator {
public:
	MallocFrameAllocator(size_t frame_size, size_t num_queued_frames);
	Frame alloc_frame() override;
	void release_frame(Frame frame) override;

private:
	size_t frame_size;

	std::mutex freelist_mutex;
	std::stack<std::unique_ptr<uint8_t[]>> freelist;  // All of size <frame_size>.
};

// Represents an input mode you can tune a card to.
struct VideoMode {
	std::string name;
	bool autodetect = false;  // If true, all the remaining fields are irrelevant.
	unsigned width = 0, height = 0;
	unsigned frame_rate_num = 0, frame_rate_den = 0;
	bool interlaced = false;
};

// Represents the format of an actual frame coming in.
// Note: Frame rate is _frame_ rate, not field rate. So 1080i60 gets 30/1, _not_ 60/1.
// "second_field_start" is only valid for interlaced modes. If it is 1,
// the two fields are actually stored interlaced (ie., every other line).
// If not, each field is stored consecutively, and it signifies how many lines
// from the very top of the frame there are before the second field
// starts (so it will always be >= height/2 + extra_lines_top).
struct VideoFormat {
	uint16_t id = 0;  // For debugging/logging only.
	unsigned width = 0, height = 0, second_field_start = 0;
	unsigned extra_lines_top = 0, extra_lines_bottom = 0;
	unsigned frame_rate_nom = 0, frame_rate_den = 0;
	unsigned stride = 0;  // In bytes, assuming no interleaving.
	bool interlaced = false;
	bool has_signal = false;
	bool is_connected = true;  // If false, then has_signal makes no sense.
};

struct AudioFormat {
	uint16_t id = 0;  // For debugging/logging only.
	unsigned bits_per_sample = 0;
	unsigned num_channels = 0;
	unsigned sample_rate = 48000;
};

enum PixelFormat {
	// 8-bit 4:2:2 in the standard Cb Y Cr Y order (UYVY).
	// This is the default.
	PixelFormat_8BitYCbCr,

	// 10-bit 4:2:2 in v210 order. Six pixels (six Y', three Cb,
	// three Cr) are packed into four 32-bit little-endian ints
	// in the following pattern (see e.g. the DeckLink documentation
	// for reference):
	//
	//   A  B   G   R
	// -----------------
	//   X Cr0 Y0  Cb0
	//   X  Y2 Cb2  Y1
	//   X Cb4 Y3  Cr2
	//   X  Y5 Cr4  Y4
	//
	// If you read in RGB order and ignore the unused top bits,
	// this is essentially Cb Y Cr Y order, just like UYVY is.
	//
	// Note that unlike true v210, there is no guarantee about
	// 128-byte line alignment (or lack thereof); you should check
	// the stride member of VideoFormat.
	PixelFormat_10BitYCbCr,

	// 8-bit 4:4:4:4 BGRA (in that order). bmusb itself doesn't
	// produce this, but it is useful to represent e.g. synthetic inputs.
	PixelFormat_8BitBGRA,

	// 8-bit 4:2:0, 4:2:2, 4:4:4 or really anything else, planar
	// (ie., first all Y', then all Cb, then all Cr). bmusb doesn't
	// produce this, nor does it specify a mechanism to describe
	// the precise details of the format.
	PixelFormat_8BitYCbCrPlanar,

	// These exist only so that the type is guaranteed wide enough
	// to contain values up to 127. CaptureInterface instances
	// are free to use them as they see fit for private uses.
	PixelFormat_Unused100 = 100,
	PixelFormat_Unused127 = 127
};

typedef std::function<void(uint16_t timecode,
                           FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
                           FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)>
	frame_callback_t;

typedef std::function<void(libusb_device *dev)> card_connected_callback_t;
typedef std::function<void()> card_disconnected_callback_t;

class CaptureInterface {
 public:
	virtual ~CaptureInterface() {}

	virtual std::map<uint32_t, VideoMode> get_available_video_modes() const = 0;
	virtual uint32_t get_current_video_mode() const = 0;
	virtual void set_video_mode(uint32_t video_mode_id) = 0;

	// TODO: Add a way to query this based on mode?
	virtual std::set<PixelFormat> get_available_pixel_formats() const = 0;
	virtual void set_pixel_format(PixelFormat pixel_format) = 0;
	virtual PixelFormat get_current_pixel_format() const = 0;

	virtual std::map<uint32_t, std::string> get_available_video_inputs() const = 0;
	virtual void set_video_input(uint32_t video_input_id) = 0;
	virtual uint32_t get_current_video_input() const = 0;

	virtual std::map<uint32_t, std::string> get_available_audio_inputs() const = 0;
	virtual void set_audio_input(uint32_t audio_input_id) = 0;
	virtual uint32_t get_current_audio_input() const = 0;

	// Does not take ownership.
	virtual void set_video_frame_allocator(FrameAllocator *allocator) = 0;

	virtual FrameAllocator *get_video_frame_allocator() = 0;

	// Does not take ownership.
	virtual void set_audio_frame_allocator(FrameAllocator *allocator) = 0;

	virtual FrameAllocator *get_audio_frame_allocator() = 0;

	virtual void set_frame_callback(frame_callback_t callback) = 0;

	// Needs to be run before configure_card().
	virtual void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) = 0;

	// Only valid after configure_card().
	virtual std::string get_description() const = 0;

	virtual void configure_card() = 0;

	virtual void start_bm_capture() = 0;

	virtual void stop_dequeue_thread() = 0;

	// If a card is disconnected, it cannot come back; you should call stop_dequeue_thread()
	// and delete it.
	virtual bool get_disconnected() const = 0;
};

// The actual capturing class, representing capture from a single card.
class BMUSBCapture : public CaptureInterface {
 public:
	BMUSBCapture(int card_index, libusb_device *dev = nullptr)
		: card_index(card_index), dev(dev)
	{
	}

	~BMUSBCapture();

	// Note: Cards could be unplugged and replugged between this call and
	// actually opening the card (in configure_card()).
	static unsigned num_cards();

	std::set<PixelFormat> get_available_pixel_formats() const override
	{
		return std::set<PixelFormat>{ PixelFormat_8BitYCbCr, PixelFormat_10BitYCbCr };
	}

	void set_pixel_format(PixelFormat pixel_format) override;

	PixelFormat get_current_pixel_format() const
	{
		return current_pixel_format;
	}

	std::map<uint32_t, VideoMode> get_available_video_modes() const override;
	uint32_t get_current_video_mode() const override;
	void set_video_mode(uint32_t video_mode_id) override;

	virtual std::map<uint32_t, std::string> get_available_video_inputs() const override;
	virtual void set_video_input(uint32_t video_input_id) override;
	virtual uint32_t get_current_video_input() const override { return current_video_input; }

	virtual std::map<uint32_t, std::string> get_available_audio_inputs() const override;
	virtual void set_audio_input(uint32_t audio_input_id) override;
	virtual uint32_t get_current_audio_input() const override { return current_audio_input; }

	// Does not take ownership.
	void set_video_frame_allocator(FrameAllocator *allocator) override
	{
		video_frame_allocator = allocator;
		if (owned_video_frame_allocator.get() != allocator) {
			owned_video_frame_allocator.reset();
		}
	}

	FrameAllocator *get_video_frame_allocator() override
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(FrameAllocator *allocator) override
	{
		audio_frame_allocator = allocator;
		if (owned_audio_frame_allocator.get() != allocator) {
			owned_audio_frame_allocator.reset();
		}
	}

	FrameAllocator *get_audio_frame_allocator() override
	{
		return audio_frame_allocator;
	}

	void set_frame_callback(frame_callback_t callback) override
	{
		frame_callback = callback;
	}

	// Needs to be run before configure_card().
	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) override
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	// Only valid after configure_card().
	std::string get_description() const override {
		return description;
	}

	void configure_card() override;
	void start_bm_capture() override;
	void stop_dequeue_thread() override;
	bool get_disconnected() const override { return disconnected; }

	// TODO: It's rather messy to have these outside the interface.
	static void start_bm_thread();
	static void stop_bm_thread();

	// Hotplug event (for devices being inserted between start_bm_thread()
	// and stop_bm_thread()); entirely optional, but must be set before
	// start_bm_capture(). Note that your callback should do as little work
	// as possible, since the callback comes from the main USB handling
	// thread, which is very time-sensitive.
	//
	// The callback function transfers ownership. If you don't want to hold
	// on to the device given to you in the callback, you need to call
	// libusb_unref_device().
	static void set_card_connected_callback(card_connected_callback_t callback,
	                                        bool hotplug_existing_devices_arg = false)
	{
		card_connected_callback = callback;
		hotplug_existing_devices = hotplug_existing_devices_arg;
	}

	// Similar to set_card_connected_callback(), with the same caveats.
	// (Note that this is set per-card and not global, as it is logically
	// connected to an existing BMUSBCapture object.)
	void set_card_disconnected_callback(card_disconnected_callback_t callback)
	{
		card_disconnected_callback = callback;
	}

 private:
	struct QueuedFrame {
		uint16_t timecode;
		uint16_t format;
		FrameAllocator::Frame frame;
	};

	void start_new_audio_block(const uint8_t *start);
	void start_new_frame(const uint8_t *start);

	void queue_frame(uint16_t format, uint16_t timecode, FrameAllocator::Frame frame, std::deque<QueuedFrame> *q);
	void dequeue_thread_func();

	static void usb_thread_func();
	static void cb_xfr(struct libusb_transfer *xfr);
	static int cb_hotplug(libusb_context *ctx, libusb_device *dev, libusb_hotplug_event event, void *user_data);

	void update_capture_mode();

	std::string description;

	FrameAllocator::Frame current_video_frame;
	FrameAllocator::Frame current_audio_frame;

	std::mutex queue_lock;
	std::condition_variable queues_not_empty;
	std::deque<QueuedFrame> pending_video_frames;
	std::deque<QueuedFrame> pending_audio_frames;

	FrameAllocator *video_frame_allocator = nullptr;
	FrameAllocator *audio_frame_allocator = nullptr;
	std::unique_ptr<FrameAllocator> owned_video_frame_allocator;
	std::unique_ptr<FrameAllocator> owned_audio_frame_allocator;
	frame_callback_t frame_callback = nullptr;
	static card_connected_callback_t card_connected_callback;
	static bool hotplug_existing_devices;
	card_disconnected_callback_t card_disconnected_callback = nullptr;

	std::thread dequeue_thread;
	std::atomic<bool> dequeue_thread_should_quit;
	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	int current_register = 0;

	static constexpr int NUM_BMUSB_REGISTERS = 60;
	uint8_t register_file[NUM_BMUSB_REGISTERS];

	// If <dev> is nullptr, will choose device number <card_index> from the list
	// of available devices on the system. <dev> is not used after configure_card()
	// (it will be unref-ed).
	int card_index = -1;
	libusb_device *dev = nullptr;

	std::vector<libusb_transfer *> iso_xfrs;
	int assumed_frame_width = 1280;

	libusb_device_handle *devh = nullptr;
	uint32_t current_video_input = 0x00000000;  // HDMI/SDI.
	uint32_t current_audio_input = 0x00000000;  // Embedded.
	PixelFormat current_pixel_format = PixelFormat_8BitYCbCr;

	bool disconnected = false;
};

}  // namespace bmusb

#endif
