# -*- coding: utf-8 -*-
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import ctypes
import sys
import os
import threading
import numpy as np
import pyaudio
import subprocess
import time
import queue
import math
import platform

# --- OS DETECTION ---
IS_WINDOWS = (os.name == 'nt')

# --- WINDOWS SPECIFIC IMPORTS ---
if IS_WINDOWS:
    try:
        import win32pipe, win32file, win32event, win32api
    except ImportError:
        messagebox.showerror("Error", "Missing 'pywin32'. Please run: pip install pywin32")
        sys.exit(1)

# Optional dependencies
try:
    import psutil
except ImportError:
    psutil = None

try:
    import cv2
except ImportError:
    cv2 = None

from PIL import Image, ImageTk

# --- CONFIGURATION ---
INPUTS = ["HDMI", "Component", "Composite", "S-Video"]
FRAMERATES = ["50", "59.94", "60", "25", "29.97", "30", "23.98", "24"] 

if IS_WINDOWS:
    SHIM_PATH = "./shim.dll"
    VIDEO_PIPE = r'\\.\pipe\bm_video_pipe'
    AUDIO_PIPE = r'\\.\pipe\bm_audio_pipe'
    LOG_FILE = os.path.abspath("./vh_debug.log") 
else:
    SHIM_PATH = "./libshim.so"
    VIDEO_PIPE = "/tmp/bm_video.pipe"
    AUDIO_PIPE = "/tmp/bm_audio.pipe"
    LOG_FILE = "/tmp/vh_debug.log"

# --- LOAD LIBRARY SAFEGUARD ---
if not os.path.exists(SHIM_PATH):
    sys.stderr.write(f"Error: {SHIM_PATH} not found.\n")

# --- LOAD C LIBRARY ---
try:
    _shim = ctypes.CDLL(SHIM_PATH)
    
    VideoCallbackFunc = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_ubyte), ctypes.c_size_t)
    AudioCallbackFunc = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t)

    _shim.init_card.restype = ctypes.c_void_p
    _shim.configure_card.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32]
    _shim.start_capture.argtypes = [ctypes.c_void_p, VideoCallbackFunc]
    _shim.set_audio_callback.argtypes = [ctypes.c_void_p, AudioCallbackFunc]
    _shim.stop_capture.argtypes = [ctypes.c_void_p]
    LIBRARY_LOADED = True
except Exception as e:
    sys.stderr.write(f"Library Load Error: {e}\n")
    LIBRARY_LOADED = False

# --- CUSTOM WIDGETS ---

class CpuGraph(tk.Canvas):
    def __init__(self, parent, width=150, height=50, bg="black"):
        super().__init__(parent, width=width, height=height, bg=bg, highlightthickness=1, highlightbackground="gray")
        self.width = width
        self.height = height
        self.points = [0] * (width // 2)
        self.setup_grid()

    def setup_grid(self):
        self.delete("grid")
        step_x = self.width / 10
        step_y = self.height / 4
        for i in range(11):
            x = i * step_x
            self.create_line(x, 0, x, self.height, fill="#004400", tags="grid")
        for i in range(5):
            y = i * step_y
            self.create_line(0, y, self.width, y, fill="#004400", tags="grid")

    def update_graph(self, cpu_percent):
        self.points.pop(0)
        self.points.append(cpu_percent)
        self.delete("line")
        coords = []
        step_x = self.width / (len(self.points) - 1)
        for i, val in enumerate(self.points):
            x = i * step_x
            y = self.height - (val / 100.0 * self.height)
            coords.append(x); coords.append(y)
        if len(coords) >= 4:
            self.create_line(coords, fill="#00FF00", width=1.5, tags="line", smooth=True)

class VuMeter(tk.Canvas):
    def __init__(self, parent, width=200, height=20, bg="#222"):
        super().__init__(parent, width=width, height=height, bg=bg, highlightthickness=1, highlightbackground="gray")
        self.width = width
        self.height = height
        self.rects_l = []
        self.rects_r = []
        self.setup_bars()

    def setup_bars(self):
        h = self.height / 2
        num_segs = 30
        w_seg = (self.width - 4) / num_segs
        for i in range(num_segs):
            x1 = 2 + i * w_seg
            x2 = 2 + (i + 1) * w_seg - 1
            if i < num_segs * 0.7: color = "#00cc00"
            elif i < num_segs * 0.9: color = "#cccc00"
            else: color = "#cc0000"
            self.rects_l.append((self.create_rectangle(x1, 1, x2, h-1, fill="#444", outline=""), color))
            self.rects_r.append((self.create_rectangle(x1, h+1, x2, self.height-1, fill="#444", outline=""), color))

    def set_levels(self, db_l, db_r):
        def map_db(db):
            if db < -60: return 0
            if db > 0: return 30
            return int(((db + 60) / 60) * 30)
        lvl_l = map_db(db_l)
        lvl_r = map_db(db_r)
        for i, (rect, color) in enumerate(self.rects_l):
            self.itemconfig(rect, fill=color if i < lvl_l else "#444")
        for i, (rect, color) in enumerate(self.rects_r):
            self.itemconfig(rect, fill=color if i < lvl_r else "#444")

class VhApp:
    def __init__(self, root):
        self.root = root
        
        # --- LOG REDIRECTION ---
        self.setup_logging()
        
        self.root.title("VHCapture Clone - Blackmagic")
        self.root.geometry("1024x768")
        
        self.pa = pyaudio.PyAudio()
        self.audio_stream = None
        
        self.card = None
        if LIBRARY_LOADED:
            self.video_cb_ref = VideoCallbackFunc(self.on_video_frame)
            self.audio_cb_ref = AudioCallbackFunc(self.on_bm_audio_frame)
        
        self.current_video_frame = None
        self.video_lock = threading.Lock()
        
        self.is_recording = False
        self.ffmpeg_process = None
        self.rec_start_time = 0
        self.dropped_v = 0
        self.dropped_a = 0
        
        self.video_q = queue.Queue(maxsize=200) 
        self.audio_q = queue.Queue(maxsize=500)
        
        self.width = 1920
        self.height = 1080
        self.color_mode_uyvy = True 
        self.connected = False
        self.closing = False

        self.audio_gain = 1.0
        self.vu_l_db = -90
        self.vu_r_db = -90

        # Stats for FPS Auto-Detection
        self.fps_counter = 0
        self.fps_timer = 0
        self.stable_source_fps = 50 
        self.rec_total_frames_seen = 0
        
        self._last_err_time = 0 # Throttling error messages

        self.create_pipes_linux() 
        self.build_ui()
        self.refresh_audio_devices()
        self.update_loop()

    def setup_logging(self):
        if os.path.exists(LOG_FILE):
            try: os.remove(LOG_FILE)
            except: pass
        try:
            self.log_fd = open(LOG_FILE, "w")
            os.dup2(self.log_fd.fileno(), 1)
            os.dup2(self.log_fd.fileno(), 2)
        except: pass

    def open_debug_log(self):
        sys.stdout.flush()
        sys.stderr.flush()
        try:
            if IS_WINDOWS: os.startfile(LOG_FILE)
            else: subprocess.call(['xdg-open', LOG_FILE])
        except: pass

    def create_pipes_linux(self):
        if not IS_WINDOWS:
            for p in [VIDEO_PIPE, AUDIO_PIPE]:
                if not os.path.exists(p):
                    try: os.mkfifo(p)
                    except: pass

    def build_ui(self):
        self.root.grid_rowconfigure(0, weight=1)
        self.root.grid_rowconfigure(1, weight=0)
        self.root.grid_columnconfigure(0, weight=1)

        center_pane = tk.PanedWindow(self.root, orient=tk.HORIZONTAL, sashwidth=4, bg="#d4d0c8")
        center_pane.grid(row=0, column=0, sticky="nsew")

        self.preview_frame = tk.Frame(center_pane, bg="black")
        center_pane.add(self.preview_frame, minsize=400, stretch="always")
        
        self.preview_lbl = tk.Label(self.preview_frame, text="NO SIGNAL", bg="black", fg="#444", font=("Arial", 14))
        self.preview_lbl.pack(fill=tk.BOTH, expand=True)

        settings_frame = tk.Frame(center_pane, bg="#d4d0c8", width=260)
        center_pane.add(settings_frame, minsize=260, stretch="never")
        settings_frame.pack_propagate(False)

        grp_src = tk.LabelFrame(settings_frame, text="Source devices", padx=5, pady=5)
        grp_src.pack(fill=tk.X, padx=5, pady=5)
        tk.Label(grp_src, text="Video capture source:", anchor="w", bg="#d4d0c8").pack(fill=tk.X)
        self.cb_video = ttk.Combobox(grp_src, values=INPUTS, state="readonly")
        self.cb_video.set("HDMI")
        self.cb_video.pack(fill=tk.X)
        tk.Label(grp_src, text="Audio capture source:", anchor="w", bg="#d4d0c8").pack(fill=tk.X)
        self.cb_audio = ttk.Combobox(grp_src, state="readonly")
        self.cb_audio.pack(fill=tk.X)
        self.cb_audio.bind("<<ComboboxSelected>>", self.on_audio_source_change)

        grp_vid = tk.LabelFrame(settings_frame, text="Video compression", padx=5, pady=5)
        grp_vid.pack(fill=tk.X, padx=5, pady=5)
        f_v1 = tk.Frame(grp_vid, bg="#d4d0c8"); f_v1.pack(fill=tk.X)
        tk.Label(f_v1, text="Bitrate:", anchor="w", bg="#d4d0c8", width=8).pack(side=tk.LEFT)
        self.var_bitrate = tk.StringVar(value="6000")
        tk.Entry(f_v1, textvariable=self.var_bitrate, width=6).pack(side=tk.LEFT)
        tk.Label(f_v1, text="kbps", bg="#d4d0c8").pack(side=tk.LEFT, padx=2)
        f_v2 = tk.Frame(grp_vid, bg="#d4d0c8"); f_v2.pack(fill=tk.X, pady=2)
        tk.Label(f_v2, text="Frame rate:", anchor="w", bg="#d4d0c8", width=10).pack(side=tk.LEFT)
        self.var_fps = tk.StringVar(value="50") 
        ttk.Combobox(f_v2, textvariable=self.var_fps, values=FRAMERATES, width=5).pack(side=tk.LEFT)
        self.var_hide_prev = tk.BooleanVar(value=False)
        tk.Checkbutton(grp_vid, text="Hide Preview", variable=self.var_hide_prev, bg="#d4d0c8").pack(anchor="w")

        grp_aud = tk.LabelFrame(settings_frame, text="Audio compression", padx=5, pady=5)
        grp_aud.pack(fill=tk.X, padx=5, pady=5)
        f_a1 = tk.Frame(grp_aud, bg="#d4d0c8"); f_a1.pack(fill=tk.X)
        tk.Label(f_a1, text="Format:", anchor="w", bg="#d4d0c8", width=8).pack(side=tk.LEFT)
        self.cb_afmt = ttk.Combobox(f_a1, state="readonly", width=15)
        self.cb_afmt.pack(side=tk.LEFT)
        self.cb_afmt['values'] = ["44100Hz 16 bits 2 ch"]
        self.cb_afmt.current(0)
        f_a2 = tk.Frame(grp_aud, bg="#d4d0c8"); f_a2.pack(fill=tk.X, pady=2)
        tk.Label(f_a2, text="Bitrate:", anchor="w", bg="#d4d0c8", width=8).pack(side=tk.LEFT)
        self.var_abitrate = tk.StringVar(value="128")
        ttk.Combobox(f_a2, textvariable=self.var_abitrate, values=["128", "192"], width=5).pack(side=tk.LEFT)
        tk.Label(f_a2, text="kbps", bg="#d4d0c8").pack(side=tk.LEFT)
        
        ttk.Separator(grp_aud, orient="horizontal").pack(fill=tk.X, pady=6)
        f_gain = tk.Frame(grp_aud, bg="#d4d0c8"); f_gain.pack(fill=tk.X)
        tk.Label(f_gain, text="Input Vol:", bg="#d4d0c8", anchor="w").pack(side=tk.LEFT)
        self.vol_var = tk.DoubleVar(value=100)
        self.slider_vol = tk.Scale(f_gain, from_=0, to=200, orient=tk.HORIZONTAL, variable=self.vol_var, 
                                   command=self.on_gain_change, bg="#d4d0c8", highlightthickness=0, length=120)
        self.slider_vol.pack(side=tk.LEFT, padx=5)
        self.lbl_gain = tk.Label(f_gain, text="100%", bg="#d4d0c8", width=4)
        self.lbl_gain.pack(side=tk.LEFT)
        tk.Label(grp_aud, text="L/R Levels:", bg="#d4d0c8", font=("Arial", 8)).pack(anchor="w", pady=(4,0))
        self.vu_meter = VuMeter(grp_aud, width=230, height=14)
        self.vu_meter.pack(pady=2)

        grp_cpu = tk.LabelFrame(settings_frame, text="CPU usage", padx=5, pady=5)
        grp_cpu.pack(fill=tk.X, padx=5, pady=5)
        self.lbl_cpu_txt = tk.Label(grp_cpu, text="0%", anchor="e", bg="#d4d0c8")
        self.lbl_cpu_txt.pack(anchor="e")
        self.cpu_graph = CpuGraph(grp_cpu, width=220, height=50)
        self.cpu_graph.pack()
        f_opt = tk.Frame(grp_cpu, bg="#d4d0c8")
        f_opt.pack(anchor="e", pady=2)
        tk.Button(f_opt, text="Debug", command=self.open_debug_log, relief="raised", bg="#ddd").pack(side=tk.LEFT, padx=5)
        
        tk.Frame(settings_frame, bg="#d4d0c8", height=10).pack() 
        tk.Button(settings_frame, text="Open My Videos", command=self.open_videos_folder, relief="raised", height=1).pack(fill=tk.X, side=tk.BOTTOM, padx=10, pady=10)

        bot_frame = tk.Frame(self.root, bg="#d4d0c8", height=50, bd=1, relief="raised")
        bot_frame.grid(row=1, column=0, sticky="ew")
        self.btn_connect = tk.Button(bot_frame, text="Connect", bg="#008800", fg="white", font=("Arial", 10, "bold"), width=10, command=self.toggle_connection)
        self.btn_connect.pack(side=tk.LEFT, padx=5, pady=5)
        ttk.Separator(bot_frame, orient="vertical").pack(side=tk.LEFT, fill=tk.Y, padx=5, pady=5)
        self.btn_rec = tk.Button(bot_frame, text="\u25CF", fg="red", font=("Arial", 18), width=3, command=self.start_recording)
        self.btn_rec.pack(side=tk.LEFT, padx=2)
        self.btn_stop = tk.Button(bot_frame, text="\u25A0", fg="#888", font=("Arial", 18), width=3, command=self.stop_recording, state="disabled")
        self.btn_stop.pack(side=tk.LEFT, padx=2)
        tk.Label(bot_frame, text="File:", bg="#d4d0c8").pack(side=tk.LEFT, padx=(15, 0))
        self.lbl_filename = tk.Entry(bot_frame, width=35)
        self.lbl_filename.insert(0, os.path.join(os.path.expanduser("~"), "Videos", "capture.mp4"))
        self.lbl_filename.pack(side=tk.LEFT, padx=2)
        tk.Button(bot_frame, text="browse", command=self.browse_file).pack(side=tk.LEFT)
        self.lbl_timer = tk.Label(bot_frame, text="0:00:0", font=("Arial", 14), bg="#d4d0c8")
        self.lbl_timer.pack(side=tk.RIGHT, padx=10)

    # --- LOGIC ---
    def on_gain_change(self, val):
        self.audio_gain = float(val) / 100.0
        self.lbl_gain.config(text=f"{int(float(val))}%")

    def browse_file(self):
        fn = filedialog.asksaveasfilename(defaultextension=".mp4", filetypes=[("MP4 Video", "*.mp4")])
        if fn:
            self.lbl_filename.delete(0, tk.END)
            self.lbl_filename.insert(0, fn)

    def open_videos_folder(self):
        path = os.path.dirname(self.lbl_filename.get())
        if os.path.exists(path):
            if IS_WINDOWS: os.startfile(path)
            else: subprocess.call(['xdg-open', path])

    def refresh_audio_devices(self):
        devs = ["Decklink Audio Capture"]
        cnt = self.pa.get_device_count()
        for i in range(cnt):
            try:
                info = self.pa.get_device_info_by_index(i)
                if info['maxInputChannels'] > 0:
                    devs.append(f"{info['name']} ({i})")
            except: pass
        self.cb_audio['values'] = devs
        self.cb_audio.current(0)

    def on_audio_source_change(self, event):
        if self.audio_stream:
            self.audio_stream.stop_stream()
            self.audio_stream.close()
            self.audio_stream = None
        
        sel = self.cb_audio.get()
        if "Decklink" not in sel:
            try:
                idx = int(sel.split('(')[-1].strip(')'))
                self.start_system_audio(idx)
            except: pass

    def start_system_audio(self, device_index):
        def callback(in_data, frame_count, time_info, status):
            data = np.frombuffer(in_data, dtype=np.int16)
            self.on_audio_data(data)
            return (None, pyaudio.paContinue)

        self.audio_stream = self.pa.open(
            format=pyaudio.paInt16,
            channels=2,
            rate=48000,
            input=True,
            input_device_index=device_index,
            stream_callback=callback
        )
        self.audio_stream.start_stream()

    def toggle_connection(self):
        if not LIBRARY_LOADED:
            messagebox.showerror("Error", f"Library not found: {SHIM_PATH}")
            return

        if not self.connected:
            self.card = _shim.init_card()
            if not self.card:
                messagebox.showerror("Error", "Blackmagic card not found.")
                return
            
            _shim.set_audio_callback(self.card, self.audio_cb_ref)
            
            try:
                idx = INPUTS.index(self.cb_video.get())
                _shim.configure_card(self.card, idx, 0)
            except:
                _shim.configure_card(self.card, 0, 0)

            if _shim.start_capture(self.card, self.video_cb_ref):
                self.connected = True
                self.preview_lbl.config(text="Waiting for Video...", image='')
                
                # Reset Stats
                self.fps_counter = 0
                self.fps_timer = time.time()
                self.stable_source_fps = 50 

                self.cb_video.config(state="disabled")
                self.btn_connect.config(text="Disconnect", bg="#cc0000")
            else:
                messagebox.showerror("Error", "Could not start capture.")
        else:
            self.connected = False
            self.preview_lbl.config(image='', text="NO SIGNAL")
            self.vu_l_db = -90; self.vu_r_db = -90
            
            if self.is_recording:
                self.stop_recording()
                
            self.root.update() 
            time.sleep(0.1) 

            if self.card:
                try: _shim.stop_capture(self.card)
                except: pass
            self.card = None
            
            self.cb_video.config(state="readonly")
            self.btn_connect.config(text="Connect", bg="#008800")

    def show_unsupported_msg(self):
        now = time.time()
        if now - self._last_err_time < 2.0: return
        self._last_err_time = now
        
        def _ui():
            self.preview_lbl.config(image='', text="UNSUPPORTED\nRESOLUTION", bg="#440000", fg="#ff5555", font=("Arial", 20, "bold"))
        self.root.after(0, _ui)

    def on_video_frame(self, data_ptr, length):
        try:
            if self.closing or not self.connected: return

            # --- HANDLE UNSUPPORTED RESOLUTION SIGNAL FROM SHIM ---
            if length == 1:
                self.show_unsupported_msg()
                return

            if length == 0: return
            
            w, h = 0, 0
            # Basic resolution detection by buffer size
            if length >= 4147200: w, h = 1920, 1080
            elif length >= 2073600: w, h = 1920, 540 # 1080i Field
            elif length >= 1843200: w, h = 1280, 720
            elif length >= 829440: w, h = 720, 576
            elif length >= 691200: w, h = 720, 486
            else: 
                # Unknown size, ignore
                return 

            if not self.is_recording:
                self.width = w
                self.height = h

            self.fps_counter += 1
            now = time.time()
            if now - self.fps_timer >= 1.0:
                raw_fps = self.fps_counter
                self.fps_counter = 0
                self.fps_timer = now
                standards = [24, 25, 30, 50, 60]
                self.stable_source_fps = min(standards, key=lambda x:abs(x-raw_fps))
                
            expected_size = w * h * 2
            if length < expected_size: return
                
            raw_data = ctypes.string_at(data_ptr, expected_size)
            
            with self.video_lock:
                self.current_video_frame = raw_data

            if self.is_recording:
                if self.width != w or self.height != h:
                     # Res changed, could handle error here
                     pass

                try:
                    target_fps = int(float(self.var_fps.get()))
                except:
                    target_fps = 25
                
                src_fps = self.stable_source_fps
                if src_fps == 0: src_fps = 50 
                
                # Simple Frame Decimation / Duplication logic
                current_step = (self.rec_total_frames_seen * target_fps) // src_fps
                prev_step = ((self.rec_total_frames_seen - 1) * target_fps) // src_fps
                self.rec_total_frames_seen += 1
                
                if current_step > prev_step:
                    try:
                        self.video_q.put(raw_data, timeout=0.005)
                    except queue.Full:
                        self.dropped_v += 1
        
        except Exception as e:
            pass

    def on_bm_audio_frame(self, data_ptr, num_samples):
        if self.closing or not self.connected: return
        if "Decklink" in self.cb_audio.get():
            AudioArray = ctypes.c_int16 * num_samples
            data = np.ctypeslib.as_array(AudioArray.from_address(ctypes.addressof(data_ptr.contents)))
            self.on_audio_data(data)

    def on_audio_data(self, data_np):
        if self.audio_gain != 1.0:
            d_float = data_np.astype(np.float32)
            d_float *= self.audio_gain
            np.clip(d_float, -32768, 32767, out=d_float)
            data_np = d_float.astype(np.int16)

        if len(data_np) > 0:
            try:
                left = data_np[0::2]
                right = data_np[1::2]
                peak_l = np.abs(left).max() if len(left) > 0 else 0
                peak_r = np.abs(right).max() if len(right) > 0 else 0
                def to_db(val):
                    if val < 1: return -90
                    return 20 * math.log10(val / 32768.0)
                self.vu_l_db = to_db(peak_l)
                self.vu_r_db = to_db(peak_r)
            except: pass

        if self.is_recording:
            try:
                self.audio_q.put(data_np.tobytes(), timeout=0.005)
            except queue.Full:
                self.dropped_a += 1

    def start_recording(self):
        if not self.connected: 
            messagebox.showinfo("Info", "Connect first!")
            return
        filename = self.lbl_filename.get()
        if not filename: return
        
        self.btn_rec.config(state="disabled", relief="sunken")
        self.btn_stop.config(state="normal", relief="raised", fg="black")
        
        if self.var_hide_prev.get():
            self.preview_lbl.config(image='', text="RECORDING IN PROGRESS\n(Preview Hidden)", bg="#220000", fg="white")
        
        with self.video_q.mutex: self.video_q.queue.clear()
        with self.audio_q.mutex: self.audio_q.queue.clear()
        
        self.rec_start_time = time.time()
        self.rec_total_frames_seen = 0 
        self.is_recording = True
        
        fps = self.var_fps.get()
        v_bit = self.var_bitrate.get() + "k"
        a_bit = self.var_abitrate.get() + "k"
        pix_fmt = 'uyvy422'
        
        cmd = [
            'ffmpeg', '-y',
            '-f', 'rawvideo', '-vcodec', 'rawvideo', '-pixel_format', pix_fmt,
            '-video_size', f'{self.width}x{self.height}', '-framerate', fps, 
            '-thread_queue_size', '8192',
            '-i', VIDEO_PIPE,
            '-f', 's16le', '-ac', '2', '-ar', '48000', 
            '-thread_queue_size', '8192',
            '-i', AUDIO_PIPE,
            '-vsync', '0', 
            '-c:v', 'libx264', '-preset', 'ultrafast', '-b:v', v_bit, 
            '-c:a', 'aac', '-b:a', a_bit,
            filename
        ]
        
        try:
            self.ffmpeg_process = subprocess.Popen(cmd, stdin=subprocess.PIPE)
            # Daemon threads ensure they die if main process dies
            threading.Thread(target=self._video_writer_thread, daemon=True).start()
            threading.Thread(target=self._audio_writer_thread, daemon=True).start()
        except Exception as e:
            messagebox.showerror("Error", f"FFMPEG Error: {e}")
            self.stop_recording()

    def stop_recording(self):
        self.is_recording = False
        
        # Stop FFMPEG
        if self.ffmpeg_process:
            self.ffmpeg_process.terminate()
            try: self.ffmpeg_process.wait(timeout=2)
            except: self.ffmpeg_process.kill()
            self.ffmpeg_process = None
            
        self.btn_rec.config(state="normal", relief="raised")
        self.btn_stop.config(state="disabled", relief="raised", fg="#888")
        
        # Restore preview if connected
        if self.connected:
            self.preview_lbl.config(bg="black", fg="#444", text="")
        else:
            self.preview_lbl.config(bg="black", fg="#444", text="NO SIGNAL")
            
        self.lbl_timer.config(text="0:00:0")

    def _video_writer_thread(self):
        if IS_WINDOWS:
            try:
                pipe_handle = win32pipe.CreateNamedPipe(VIDEO_PIPE, win32pipe.PIPE_ACCESS_OUTBOUND, win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT, 1, 65536, 65536, 0, None)
                win32pipe.ConnectNamedPipe(pipe_handle, None)
                fd = win32file._open_osfhandle(pipe_handle, 0)
            except Exception as e:
                return
        else:
            try: fd = os.open(VIDEO_PIPE, os.O_WRONLY)
            except: return

        while self.is_recording:
            try:
                data = self.video_q.get(timeout=1)
                os.write(fd, data)
                self.video_q.task_done()
            except (queue.Empty, OSError):
                if not self.is_recording: break
        try: os.close(fd)
        except: pass

    def _audio_writer_thread(self):
        if IS_WINDOWS:
            try:
                pipe_handle = win32pipe.CreateNamedPipe(AUDIO_PIPE, win32pipe.PIPE_ACCESS_OUTBOUND, win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_WAIT, 1, 65536, 65536, 0, None)
                win32pipe.ConnectNamedPipe(pipe_handle, None)
                fd = win32file._open_osfhandle(pipe_handle, 0)
            except: return
        else:
            try: fd = os.open(AUDIO_PIPE, os.O_WRONLY)
            except: return

        while self.is_recording:
            try:
                data = self.audio_q.get(timeout=1)
                os.write(fd, data)
                self.audio_q.task_done()
            except (queue.Empty, OSError):
                if not self.is_recording: break
        try: os.close(fd)
        except: pass

    def update_loop(self):
        if self.closing: return
        self.vu_meter.set_levels(self.vu_l_db, self.vu_r_db)
        self.vu_l_db = max(self.vu_l_db - 2, -90)
        self.vu_r_db = max(self.vu_r_db - 2, -90)

        if self.is_recording:
            dur = int(time.time() - self.rec_start_time)
            h, r = divmod(dur, 3600)
            m, s = divmod(r, 60)
            self.lbl_timer.config(text=f"{h}:{m:02}:{s:02}")

        if psutil:
            cpu = psutil.cpu_percent(interval=None)
            self.lbl_cpu_txt.config(text=f"{int(cpu)}%")
            self.cpu_graph.update_graph(cpu)

        show_preview = self.connected and (cv2 is not None) and (not self.var_hide_prev.get())
        if show_preview:
            frame_data = None
            with self.video_lock:
                if self.current_video_frame:
                    frame_data = self.current_video_frame
                    self.current_video_frame = None
            if frame_data:
                try:
                    expected = self.width * self.height * 2
                    if len(frame_data) >= expected:
                        raw = np.frombuffer(frame_data[:expected], dtype=np.uint8)
                        yuv = raw.reshape((self.height, self.width, 2))
                        rgb = cv2.cvtColor(yuv, cv2.COLOR_YUV2RGB_UYVY)
                        w_win, h_win = self.preview_frame.winfo_width(), self.preview_frame.winfo_height()
                        if w_win > 10 and h_win > 10:
                            rat = self.width / self.height
                            h_new = int(w_win / rat)
                            if h_new > h_win:
                                h_new = h_win; w_new = int(h_new * rat)
                            else: w_new = w_win
                            rgb_s = cv2.resize(rgb, (w_new, h_new))
                            img = ImageTk.PhotoImage(image=Image.fromarray(rgb_s))
                            self.preview_lbl.configure(image=img, text=""); self.preview_lbl.image = img
                except: pass
        elif self.connected and self.var_hide_prev.get(): 
            pass # Preview handled by toggles
        
        self.root.after(40, self.update_loop)

    def on_close(self):
        self.closing = True
        self.connected = False 
        
        if self.is_recording: 
            self.stop_recording()
        
        # Try to stop C++ card safely
        if self.card: 
            try: _shim.stop_capture(self.card)
            except: pass
            self.card = None

        if self.audio_stream: 
            try:
                self.audio_stream.stop_stream()
                self.audio_stream.close()
            except: pass
        
        try: self.pa.terminate()
        except: pass
        
        try: self.root.destroy()
        except: pass

        # Force kill process to ensure all threads die
        # This solves the "window closes but script hangs" issue
        os._exit(0)

if __name__ == "__main__":
    root = tk.Tk()
    app = VhApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()   
