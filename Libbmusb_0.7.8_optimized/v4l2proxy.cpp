//
// A helper proxy to send data from bmusb to a V4L2 output.
// To get it as a V4L2 _input_, you can use v4l2loopback:
//
//   sudo apt install v4l2loopback-dkms v4l2loopback-utils
//   sudo modprobe v4l2loopback video_nr=2 card_label='Intensity Shuttle (bmusb)' max_width=1280 max_height=720 exclusive_caps=1
//   ./bmusb-v4l2proxy /dev/video2
//
// There is currently no audio support.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <bmusb/bmusb.h>
#if __SSE2__
#include <immintrin.h>
#endif
#include <algorithm>

using namespace std;
using namespace bmusb;

BMUSBCapture *usb;
int video_fd;

void frame_callback(uint16_t timecode,
                    FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
                    FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	printf("0x%04x: %zu video bytes (format 0x%04x, %d x %d)\n",
		timecode,
		video_frame.len - video_offset, video_format.id, video_format.width, video_format.height);

	static unsigned last_width = 0, last_height = 0, last_stride = 0;

	if (video_format.width != last_width ||
	    video_format.height != last_height ||
	    video_format.stride != last_stride) {
		v4l2_format fmt;
		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		fmt.fmt.pix.width = video_format.width;
		fmt.fmt.pix.height = video_format.height;

		// Chrome accepts YUYV, but not our native UYVY. We byteswap below.
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;

		fmt.fmt.pix.field = V4L2_FIELD_NONE;
		fmt.fmt.pix.bytesperline = video_format.stride;
		fmt.fmt.pix.sizeimage = video_format.stride * video_format.height;
		fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		int err = ioctl(video_fd, VIDIOC_S_FMT, &fmt);
		if (err == -1) {
			perror("ioctl(VIDIOC_S_FMT)");
			usb->get_video_frame_allocator()->release_frame(video_frame);
			usb->get_audio_frame_allocator()->release_frame(audio_frame);
			return;
		} else {
			last_width = video_format.width;
			last_height = video_format.height;
			last_stride = video_format.stride;
		}
	}

	if (video_frame.data != nullptr) {
		uint8_t *origptr = video_frame.data + video_offset + video_format.extra_lines_top * video_format.stride;
#if __SSE2__
		__m128i *ptr = (__m128i *)origptr;
		for (unsigned i = 0; i < video_format.stride * video_format.height / 16; ++i) {
			__m128i val = _mm_loadu_si128(ptr);
			val = _mm_slli_epi16(val, 8) | _mm_srli_epi16(val, 8);
			_mm_storeu_si128(ptr, val);
			++ptr;
		}
#else
		uint8_t *ptr = origptr;
		for (unsigned i = 0; i < video_format.stride * video_format.height / 4; ++i) {
			swap(ptr[0], ptr[1]);
			swap(ptr[2], ptr[3]);
			ptr += 4;
		}
#endif

		size_t len = video_frame.len;
		while (len > 0) {
			ssize_t ret = write(video_fd, origptr, len);
			if (ret == -1) {
				if (errno == EINTR) {
					continue;
				} else {
					perror("write");
					break;  // Hope for better luck next frame.
				}
			}
			origptr += ret;
			len -= ret;
		}
	}

	usb->get_video_frame_allocator()->release_frame(video_frame);
	usb->get_audio_frame_allocator()->release_frame(audio_frame);
}

int main(int argc, char **argv)
{
	const char *filename = (argc >= 2) ? argv[1] : "/dev/video2";
	video_fd = open(filename, O_RDWR);
	if (video_fd == -1) {
		perror(filename);
		exit(1);
	}

	usb = new BMUSBCapture(0);  // First card.
	usb->set_frame_callback(frame_callback);
	usb->configure_card();
	BMUSBCapture::start_bm_thread();
	usb->start_bm_capture();
	sleep(1000000);
}

