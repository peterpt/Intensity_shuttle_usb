#include <stdio.h>
#include <unistd.h>
#include "bmusb/bmusb.h"

using namespace std;
using namespace bmusb;
	
BMUSBCapture *usb;

void check_frame_stability(uint16_t timecode,
                           FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
                           FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	//printf("0x%04x: %d video bytes (format 0x%04x), %d audio bytes (format 0x%04x)\n",
	//	timecode, video_end - video_start, video_format.id, audio_end - audio_start, audio_format.id);

	static uint16_t last_timecode = 0;
	static size_t last_video_bytes = 0;
	static size_t last_audio_bytes = 0;

	if (!(last_timecode == 0 && last_video_bytes == 0 && last_audio_bytes == 0)) {
		if (timecode != (uint16_t)(last_timecode + 1)) {
			printf("0x%04x: Dropped %d frames\n", timecode, timecode - last_timecode - 1);
		} else if (last_video_bytes != video_frame.len - video_offset) {
			printf("0x%04x: Video frame size changed (old=%ld, cur=%ld)\n", timecode,
				last_video_bytes, video_frame.len - video_offset);
		} else if (last_audio_bytes != audio_frame.len - audio_offset) {
			printf("0x%04x: Audio block size changed (old=%ld, cur=%ld)\n", timecode,
				last_audio_bytes, audio_frame.len - audio_offset);
		}
	}
	last_timecode = timecode;
	last_video_bytes = video_frame.len - video_offset;
	last_audio_bytes = audio_frame.len - audio_offset;

	usb->get_video_frame_allocator()->release_frame(video_frame);
	usb->get_audio_frame_allocator()->release_frame(audio_frame);
}

int main(int argc, char **argv)
{
	usb = new BMUSBCapture(0);  // First card.
	usb->set_frame_callback(check_frame_stability);
	usb->configure_card();
	BMUSBCapture::start_bm_thread();
	usb->start_bm_capture();
	sleep(1000000);
}

