#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Minimal Apple Live Photo (.livp) viewer.

Shows the still image; top-left LIVE button plays the embedded MOV.

Usage:
  python livp_viewer.py
  python livp_viewer.py path/to/photo.livp
"""

from __future__ import annotations

import io
import os
import sys
import tempfile
import threading
import zipfile
from pathlib import Path
from typing import Optional, Tuple

import tkinter as tk
from tkinter import filedialog, messagebox

from PIL import Image, ImageTk

try:
    from pillow_heif import register_heif_opener

    register_heif_opener()
except Exception:
    pass

try:
    import cv2
except Exception as e:  # pragma: no cover
    cv2 = None
    _cv2_err = e


STILL_EXTS = {".jpg", ".jpeg", ".heic", ".heif", ".png", ".tif", ".tiff"}
VIDEO_EXTS = {".mov", ".mp4", ".m4v"}


def _score_still(name: str) -> int:
    ext = Path(name).suffix.lower()
    if ext in (".heic", ".heif"):
        return 3
    if ext in (".jpg", ".jpeg"):
        return 2
    if ext in STILL_EXTS:
        return 1
    return -1


def extract_livp(path: Path) -> Tuple[Image.Image, bytes, str]:
    """Return (still_image, video_bytes, video_suffix)."""
    with zipfile.ZipFile(path, "r") as zf:
        still_name = None
        still_score = -1
        video_name = None
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename
            base = Path(name).name
            ext = Path(base).suffix.lower()
            s = _score_still(base)
            if s > still_score:
                still_score = s
                still_name = name
            if ext in VIDEO_EXTS and video_name is None:
                video_name = name

        if not still_name:
            raise ValueError("No still image found inside .livp")
        if not video_name:
            raise ValueError("No MOV/MP4 found inside .livp")

        still_bytes = zf.read(still_name)
        video_bytes = zf.read(video_name)
        img = Image.open(io.BytesIO(still_bytes))
        img.load()
        if img.mode not in ("RGB", "RGBA"):
            img = img.convert("RGB")
        return img, video_bytes, Path(video_name).suffix.lower() or ".mov"


class LivpViewer(tk.Tk):
    def __init__(self, initial: Optional[Path] = None):
        super().__init__()
        self.title("LIVP Viewer")
        self.geometry("960x640")
        self.minsize(480, 320)
        self.configure(bg="#111111")

        self._still: Optional[Image.Image] = None
        self._photo: Optional[ImageTk.PhotoImage] = None
        self._video_bytes: Optional[bytes] = None
        self._video_suffix = ".mov"
        self._playing = False
        self._play_stop = threading.Event()
        self._temp_video: Optional[Path] = None
        self._path: Optional[Path] = None

        self._build_ui()
        self.bind("<Configure>", self._on_resize)
        self.bind("<Control-o>", lambda e: self.open_dialog())
        self.bind("<Escape>", lambda e: self._stop_playback())
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        if initial:
            self.after(50, lambda: self.load_path(initial))

    def _build_ui(self) -> None:
        bar = tk.Frame(self, bg="#1a1a1a", height=36)
        bar.pack(side=tk.TOP, fill=tk.X)
        bar.pack_propagate(False)

        tk.Button(
            bar,
            text="打开…",
            command=self.open_dialog,
            bg="#2a2a2a",
            fg="#eee",
            activebackground="#3a3a3a",
            activeforeground="#fff",
            relief=tk.FLAT,
            padx=12,
        ).pack(side=tk.LEFT, padx=8, pady=4)

        self._status = tk.Label(
            bar, text="打开一个 .livp 文件", bg="#1a1a1a", fg="#aaa", anchor="w"
        )
        self._status.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=8)

        self._stage = tk.Frame(self, bg="#0d0d0d")
        self._stage.pack(fill=tk.BOTH, expand=True)

        self._canvas = tk.Canvas(self._stage, bg="#0d0d0d", highlightthickness=0)
        self._canvas.pack(fill=tk.BOTH, expand=True)

        # LIVE sits on top-left of the image stage
        self._live_btn = tk.Button(
            self._stage,
            text="LIVE",
            font=("Segoe UI Semibold", 11),
            bg="#f2f2f2",
            fg="#111",
            activebackground="#ffffff",
            activeforeground="#000",
            relief=tk.FLAT,
            padx=10,
            pady=2,
            cursor="hand2",
            command=self.toggle_live,
        )
        self._live_visible = False

    def _show_live_btn(self) -> None:
        self._live_btn.place(x=16, y=16)
        self._live_visible = True
        self._live_btn.lift()

    def _hide_live_btn(self) -> None:
        self._live_btn.place_forget()
        self._live_visible = False

    def open_dialog(self) -> None:
        path = filedialog.askopenfilename(
            title="选择 Live Photo (.livp)",
            filetypes=[("Apple Live Photo", "*.livp"), ("All files", "*.*")],
        )
        if path:
            self.load_path(Path(path))

    def load_path(self, path: Path) -> None:
        self._stop_playback()
        path = path.resolve()
        try:
            still, video_bytes, suffix = extract_livp(path)
        except Exception as e:
            messagebox.showerror("无法打开", f"{path.name}\n\n{e}")
            return

        self._path = path
        self._still = still.convert("RGB")
        self._video_bytes = video_bytes
        self._video_suffix = suffix
        self.title(f"LIVP Viewer — {path.name}")
        self._status.config(
            text=f"{path.name}  ·  {still.width}×{still.height}  ·  视频 {len(video_bytes) // 1024} KB"
        )
        self._show_live_btn()
        self._set_live_idle()
        self._show_still()

    def _fit_image(self, im: Image.Image) -> Image.Image:
        cw = max(self._canvas.winfo_width(), 1)
        ch = max(self._canvas.winfo_height(), 1)
        scale = min(cw / im.width, ch / im.height, 1.0)
        if scale < 0.999:
            nw = max(1, int(im.width * scale))
            nh = max(1, int(im.height * scale))
            return im.resize((nw, nh), Image.Resampling.LANCZOS)
        return im

    def _show_pil(self, im: Image.Image) -> None:
        fitted = self._fit_image(im)
        self._photo = ImageTk.PhotoImage(fitted)
        self._canvas.delete("frame")
        cw = self._canvas.winfo_width()
        ch = self._canvas.winfo_height()
        self._canvas.create_image(cw // 2, ch // 2, image=self._photo, tags="frame")
        if self._live_visible:
            self._live_btn.lift()

    def _show_still(self) -> None:
        if self._still is not None:
            self._show_pil(self._still)

    def _on_resize(self, _event=None) -> None:
        if self._playing:
            return
        if self._still is not None:
            self._show_still()

    def _set_live_idle(self) -> None:
        self._live_btn.config(text="LIVE", bg="#f2f2f2", fg="#111")

    def _set_live_playing(self) -> None:
        self._live_btn.config(text="■", bg="#ff3b30", fg="#fff")

    def toggle_live(self) -> None:
        if self._playing:
            self._stop_playback()
        else:
            self._start_playback()

    def _cleanup_temp(self) -> None:
        if self._temp_video and self._temp_video.exists():
            try:
                self._temp_video.unlink()
            except OSError:
                pass
        self._temp_video = None

    def _start_playback(self) -> None:
        if cv2 is None:
            messagebox.showerror("缺少依赖", f"需要 opencv-python\n{_cv2_err}")
            return
        if not self._video_bytes or self._playing:
            return

        self._cleanup_temp()
        fd, tmp = tempfile.mkstemp(suffix=self._video_suffix, prefix="livp_")
        os.close(fd)
        self._temp_video = Path(tmp)
        self._temp_video.write_bytes(self._video_bytes)

        self._play_stop.clear()
        self._playing = True
        self._set_live_playing()
        threading.Thread(
            target=self._play_thread, args=(str(self._temp_video),), daemon=True
        ).start()

    def _stop_playback(self) -> None:
        self._play_stop.set()
        self._playing = False
        self._set_live_idle()
        self.after(0, self._show_still)

    def _play_thread(self, video_path: str) -> None:
        cap = cv2.VideoCapture(video_path)
        if not cap.isOpened():
            self.after(
                0,
                lambda: messagebox.showerror(
                    "播放失败", "无法解码 MOV（可能缺少 HEVC 解码器）"
                ),
            )
            self.after(0, self._stop_playback)
            return

        fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
        delay = max(1, int(1000 / fps))

        while not self._play_stop.is_set():
            ok, frame = cap.read()
            if not ok:
                break
            frame = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
            im = Image.fromarray(frame)
            self.after(0, lambda img=im: self._show_pil(img) if self._playing else None)
            if self._play_stop.wait(delay / 1000.0):
                break

        cap.release()
        self.after(0, self._stop_playback)

    def _on_close(self) -> None:
        self._play_stop.set()
        self._cleanup_temp()
        self.destroy()


def main() -> None:
    initial = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    app = LivpViewer(initial)
    app.mainloop()


if __name__ == "__main__":
    main()
