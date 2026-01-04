#VHCapture for linux

- This project was build using google ai assistance over multiples sessions
this project uses a modiefied wrapper of libbmusb that we had to adjust for the
multiples resolutions , this driver is native and it will work with your older
i5 or amd64 cpu .

These are the changes in libbmusb according to ai :
Summary of Patches Applied to libbmusb-0.7.8

The original libbmusb-0.7.8 source code is a fantastic starting point but contains several critical issues that prevent stable, long-term operation, especially with modern HD resolutions and repeated use. The following patches have been applied to resolve these issues.
1. Fix: Crash on Reconnecting (Memory and Resource Leak)

    Problem: After a successful connect/disconnect cycle, attempting to connect a second time would cause a SIGABRT or SIGSEGV inside libusb.

    Cause: The original BMUSBCapture class had no destructor (~BMUSBCapture). It never cancelled or freed the libusb_transfer objects or closed the libusb device handle. These resources would leak and remain "active" from libusb's perspective, leading to a use-after-free error when the application tried to initialize the card again.

    Solution:

        Declared the destructor ~BMUSBCapture(); in bmusb.h.

        Implemented the destructor in bmusb.cpp. This destructor now correctly cleans up all resources in the proper order:

            Stops and joins the dequeue_thread and usb_thread.

            Closes the libusb device handle (libusb_close).

            Frees all allocated libusb_transfer objects (libusb_free_transfer).

2. Fix: 1080p Signal Lock Failure (USB Overflow)

    Problem: When providing a 1080p signal (e.g., 1920x1080 @ 30Hz), the driver would fail to lock on, spamming Error: pack ... status 6 (which corresponds to LIBUSB_TRANSFER_OVERFLOW) errors.

    Cause: This was a "chicken-and-egg" problem. The driver starts by assuming a 720p resolution (assumed_frame_width = 1280) and allocates USB packet buffers optimized for that size (~15KB). However, the hardware immediately sends a much larger burst of data for the 1080p signal. This overflows the small buffer before the driver can ever read the frame's header to learn that the signal is actually 1080p. It remains stuck in this failed state, repeatedly allocating buffers that are too small.

    Solution:

        Modified the find_xfer_size_for_width function in bmusb.cpp to be more aggressive. It now allocates a large, safe buffer size of 32768 bytes (32KB) for any HD signal (i.e., when width >= 1280).

        This buffer is large enough to successfully receive the first 1080p frame without overflowing, allowing the driver to correctly read the header, detect the true resolution, and lock onto the signal. The same buffer size works fine for 720p signals (it just won't be filled completely).

3. Feature: Graceful Handling of Unsupported Resolutions

    Problem: An unsupported video signal would cause the driver to either drop frames or incorrectly assume a 720p format, leading to garbage data in the application or a potential crash.

    Cause: The decode_video_format function had a fallback that was not helpful for the application layer.

    Solution:

        Modified the fallback case at the end of the decode_video_format function in bmusb.cpp.

        Instead of assuming 720p, it now returns a "magic" resolution of 2x2 pixels.

        This acts as an explicit error signal that can be detected by the C++ shim (shim.cpp), which in turn signals the Python GUI to display an "Unsupported Resolution" message to the user instead of crashing.


This app does not require a driver from blackmagic for linux , but it requires the reneseas usb3 chipset required from blackmagic .

