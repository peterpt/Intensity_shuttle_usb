#include <bmusb/bmusb.h>
#include <iostream>
#include <vector>

typedef void (*PythonVideoCallback)(uint8_t* v_data, size_t v_len);
typedef void (*PythonAudioCallback)(int16_t* a_data, size_t num_samples);

struct Wrapper {
    bmusb::BMUSBCapture* cap = nullptr;
    PythonVideoCallback py_video_cb = nullptr;
    PythonAudioCallback py_audio_cb = nullptr;
    std::vector<int16_t> audio_buffer;
};

extern "C" {
    void* init_card() {
        try {
            if (bmusb::BMUSBCapture::num_cards() == 0) return nullptr;
            auto* w = new Wrapper();
            // Initialize with card index 0
            w->cap = new bmusb::BMUSBCapture(0);
            w->audio_buffer.reserve(4096);
            return (void*)w;
        } catch (...) {
            return nullptr;
        }
    }

    void configure_card(void* ptr, int v_input_index, uint32_t ignored) {
        Wrapper* w = (Wrapper*)ptr;
        if (!w || !w->cap) return;
        try {
            // configure_card() internally starts the 'dequeue_thread'
            w->cap->configure_card(); 
            
            uint32_t video_id = 0, audio_id = 0;
            // Map inputs based on Blackmagic specifications
            if (v_input_index == 1) { video_id = 0x02000000; audio_id = 0x10000000; } // Component
            else if (v_input_index == 2) { video_id = 0x04000000; audio_id = 0x10000000; } // Composite
            else if (v_input_index == 3) { video_id = 0x06000000; audio_id = 0x10000000; } // S-Video
            w->cap->set_video_input(video_id);
            w->cap->set_audio_input(audio_id);
            // Mode 0 = Autodetect
            w->cap->set_video_mode(0);
        } catch (...) {}
    }

    void set_audio_callback(void* ptr, PythonAudioCallback cb) {
        Wrapper* w = (Wrapper*)ptr; 
        if (w) w->py_audio_cb = cb;
    }

    int start_capture(void* ptr, PythonVideoCallback video_cb) {
        Wrapper* w = (Wrapper*)ptr;
        if (!w || !w->cap) return 0;

        w->py_video_cb = video_cb;
        
        // Register the lambda callback
        // Note: We capture 'fmt' (VideoFormat) to check resolution details
        w->cap->set_frame_callback([w](uint16_t, bmusb::FrameAllocator::Frame vf, size_t vl, bmusb::VideoFormat fmt, bmusb::FrameAllocator::Frame af, size_t al, bmusb::AudioFormat) {
            if (!w) return;

            // --- VIDEO HANDLING ---
            if (w->py_video_cb) {
                // Check for the "Unsupported Resolution" flag set in bmusb.cpp
                if (fmt.width == 2 && fmt.height == 2) {
                    // Send a 1-byte signal to Python indicating an error
                    uint8_t error_sig = 0xFF; 
                    w->py_video_cb(&error_sig, 1); 
                } 
                else {
                    // Standard Video Processing
                    size_t video_len = (vf.len > vl) ? (vf.len - vl) : 0;
                    if (video_len > 0) w->py_video_cb(vf.data + vl, video_len);
                }
            }

            // --- AUDIO HANDLING ---
            if (w->py_audio_cb && af.data && al > 0) {
                size_t audio_len = (af.len > al) ? (af.len - al) : 0;
                if (audio_len > 0) {
                    uint8_t* audio_ptr = af.data + al;
                    size_t num_frames = audio_len / 24;
                    
                    // Resize vector if necessary
                    if (w->audio_buffer.size() < num_frames * 2) {
                        w->audio_buffer.resize(num_frames * 2);
                    }
                    
                    // Convert 24-bit raw to 16-bit
                    for (size_t i = 0; i < num_frames; ++i) {
                        size_t offset = i * 24;
                        // Taking the upper 2 bytes of the 3-byte sample
                        w->audio_buffer[i*2]   = (int16_t)((audio_ptr[offset + 1]) | (audio_ptr[offset + 2] << 8));
                        w->audio_buffer[i*2+1] = (int16_t)((audio_ptr[offset + 4]) | (audio_ptr[offset + 5] << 8));
                    }
                    
                    if (num_frames > 0) {
                        w->py_audio_cb(w->audio_buffer.data(), num_frames * 2);
                    }
                }
            }
            
            // Release frames back to the allocator
            if (vf.owner) vf.owner->release_frame(vf);
            if (af.owner) af.owner->release_frame(af);
        });

        try {
            // start_bm_thread starts the global USB poll thread
            w->cap->start_bm_thread();
            // start_bm_capture submits the initial USB transfer requests
            w->cap->start_bm_capture();
            return 1;
        } catch (...) { return 0; }
    }

    void stop_capture(void* ptr) {
        Wrapper* w = (Wrapper*)ptr;
        if (w && w->cap) {
            try {
                // 1. Disable callbacks immediately
                w->py_video_cb = nullptr;
                w->py_audio_cb = nullptr;

                // 2. STOP THE DEQUEUE THREAD (Queue Consumer)
                // This function joins the thread, ensuring no callbacks are running.
                w->cap->stop_dequeue_thread();

                // 3. STOP THE GLOBAL USB THREAD (Data Producer)
                w->cap->stop_bm_thread();

                // 4. Clean up memory
                // Safe to delete now that threads are joined.
                // The updated bmusb.cpp destructor will handle closing libusb handles.
                delete w->cap;
                w->cap = nullptr;
                delete w;
            } catch (...) {
                // Swallow errors during shutdown to avoid crash
            }
        }
    }
}
