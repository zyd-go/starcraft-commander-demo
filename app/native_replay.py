"""Native StarCraft replay session and a small desktop timeline."""
from pathlib import Path
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk

from matches import ROOT, GameProcesses, game_lock, new_run, prepare, read_json, store
from replay_info import describe, load_labels


class NativeReplay:
    def __init__(self, replay, fullscreen, update):
        self.replay = Path(replay).resolve(strict=True)
        if self.replay.suffix.lower() != ".rep" or not self.replay.stat().st_size:
            raise ValueError("找不到有效的 .rep 录像。")
        self.fullscreen = fullscreen
        self.update = update
        self.commands = queue.Queue()
        self.stopping = threading.Event()
        self.directory = new_run(ROOT / "playback")
        self.paused = False
        self.speed = 42
        self.labels = load_labels(self.replay)

    def request(self, action, frame=0, paused=False, speed=42):
        self.commands.put((action, int(frame), bool(paused), int(speed)))

    def close(self):
        self.stopping.set()

    def run(self):
        with game_lock():
            self.directory.mkdir(parents=True)
            store(self.directory / "replay.json", {"source": str(self.replay), "player": "StarCraft 1.16.1"})
            target = 0
            generation = 0
            while not self.stopping.is_set():
                slot = self.directory / f"load-{generation:03d}"
                env = prepare(slot, None, "", self.speed, "baseline",
                              fullscreen=self.fullscreen, replay=self.replay)
                env.update(REPLAY_INITIAL_FRAME=str(target),
                           REPLAY_INITIAL_PAUSED="1" if self.paused else "0",
                           REPLAY_INITIAL_SPEED=str(self.speed))
                games = GameProcesses()
                self.update({"phase": "loading", "frame": 0, "target": target, "paused": self.paused})
                try:
                    process = games.launch(slot, env)
                    status_file = slot / "bwapi-data/write/replay-status.json"
                    deadline = time.monotonic() + 60
                    sequence = 0
                    state = None
                    identity_loaded = False
                    reload_requested = False
                    while not self.stopping.wait(.08):
                        observed = read_json(status_file)
                        if observed:
                            if not identity_loaded:
                                identity = read_json(slot / "bwapi-data/write/match-identity.json")
                                if identity:
                                    self.labels.update({key: identity[key] for key in ("map_actual", "players")})
                                    store(self.directory / "replay.json", {"source": str(self.replay),
                                          "player": "StarCraft 1.16.1", "labels": self.labels})
                                    identity_loaded = True
                            state = observed
                            self.update(dict(observed, phase="ready", labels=self.labels))
                        elif time.monotonic() > deadline:
                            raise RuntimeError("原版游戏未能载入这份录像。")
                        if process.poll() is not None:
                            return
                        if not state:
                            continue
                        # Coalesce a series of slider releases to the latest request.
                        pending = None
                        while not self.commands.empty():
                            pending = self.commands.get_nowait()
                        if pending:
                            action, requested, self.paused, self.speed = pending
                            total = max(0, state["total"] - 1)
                            if action == "seek":
                                target = max(0, min(total, requested))
                                reload_requested = target < state["frame"] or state["ended"]
                            elif state["ended"] and not self.paused:
                                target = 0
                                reload_requested = True
                            if reload_requested:
                                break
                            sequence += 1
                            command = slot / "bwapi-data/read/replay-command.txt"
                            temporary = command.with_suffix(".tmp")
                            temporary.write_text(
                                f"{sequence} {action} {target} {int(self.paused)} {self.speed}\n", encoding="ascii")
                            temporary.replace(command)
                        if state.get("reload"):
                            target = state["target"]
                            reload_requested = True
                            break
                    if not reload_requested:
                        return
                finally:
                    games.close()
                generation += 1


def replay_time(frame):
    seconds = max(0, int(frame)) * 42 // 1000
    minutes, seconds = divmod(seconds, 60)
    hours, minutes = divmod(minutes, 60)
    return f"{hours:d}:{minutes:02d}:{seconds:02d}" if hours else f"{minutes:02d}:{seconds:02d}"


class ReplayPanel:
    def __init__(self, parent, replay, fullscreen, finished):
        self.parent = parent
        self.finished = finished
        self.events = queue.Queue()
        self.dragging = False
        self.total = 0
        self.current = 0
        self.paused = False
        self.closed = False
        self.ended = False
        self.session = NativeReplay(replay, fullscreen, self.events.put)
        self.window = tk.Toplevel(parent)
        self.window.title("StarCraft 原版回放进度")
        self.window.attributes("-topmost", True)
        width = min(self.window.winfo_screenwidth() - 24, 480)
        left = (self.window.winfo_screenwidth() - width) // 2
        # Keep controls inside the game's cursor bounds, over the lower console.
        # The letterbox above the viewport is outside cnc-ddraw's mouse capture.
        top = max(24, self.window.winfo_screenheight() - 140)
        self.window.geometry(f"{width}x64+{left}+{top}")
        self.window.resizable(False, False)
        self.window.configure(bg="#192432")
        self.window.protocol("WM_DELETE_WINDOW", self.close)
        self.status = tk.StringVar(value="正在载入原版游戏录像…")
        self.details = tk.StringVar(value=describe(self.session.labels))
        tk.Label(self.window, textvariable=self.details, fg="#f1f5f9", bg="#192432",
                 font=("Microsoft YaHei UI", 8), wraplength=width - 16,
                 anchor="w", justify="left").pack(fill="x", padx=8, pady=(3, 0))
        style = ttk.Style(self.window)
        style.configure("Replay.TCombobox", font=("Microsoft YaHei UI", 8),
                        padding=(1, 0), arrowsize=10)
        style.configure("Replay.Horizontal.TScale", background="#5edbc3",
                        troughcolor="#3b5063", borderwidth=0)
        row = tk.Frame(self.window, bg="#192432")
        row.pack(fill="both", expand=True, padx=4, pady=3)
        grip = tk.Label(row, text="⋮⋮", fg="#91a4b7", bg="#192432",
                        font=("Microsoft YaHei UI", 8), cursor="fleur", padx=1)
        grip.pack(side="left")

        def button(text, command, width):
            return tk.Button(row, text=text, command=command, width=width,
                             font=("Microsoft YaHei UI", 8), relief="flat", borderwidth=0,
                             bg="#293a4c", fg="#f1f5f9", activebackground="#3b5067",
                             activeforeground="#ffffff", disabledforeground="#8396a7",
                             cursor="hand2", padx=2, pady=0, highlightthickness=0)

        self.play_button = button("暂停", self.toggle_pause, 4)
        self.play_button.configure(state="disabled")
        self.play_button.pack(side="left", padx=(2, 4))
        self.speed = tk.StringVar(value="1×")
        speed = ttk.Combobox(row, style="Replay.TCombobox", state="readonly", width=3,
                             font=("Microsoft YaHei UI", 8), textvariable=self.speed,
                             values=("0.5×", "1×", "2×", "4×", "8×"))
        speed.pack(side="left", padx=(0, 5))
        speed.bind("<<ComboboxSelected>>", lambda _: self.send_playback())
        self.close_button = button("×", self.close, 2)
        self.close_button.pack(side="right")
        status = tk.Label(row, textvariable=self.status, fg="#f1f5f9", bg="#192432",
                          font=("Microsoft YaHei UI", 8), width=19, cursor="fleur")
        status.pack(side="right", padx=4)
        self.progress = ttk.Scale(row, style="Replay.Horizontal.TScale", from_=0, to=1,
                                  orient="horizontal", state="disabled")
        self.progress.pack(side="left", fill="x", expand=True, padx=(0, 3))
        for handle in (row, grip, status):
            handle.bind("<ButtonPress-1>", self.begin_move)
            handle.bind("<B1-Motion>", self.move)
        self.progress.bind("<ButtonPress-1>", self.begin_drag)
        self.progress.bind("<ButtonRelease-1>", self.end_drag)
        self.progress.bind("<B1-Motion>", self.preview)
        self.progress.bind("<KeyRelease-Left>", self.keyboard_seek)
        self.progress.bind("<KeyRelease-Right>", self.keyboard_seek)

        def play():
            try:
                self.session.run()
                self.events.put({"phase": "closed"})
            except Exception as error:
                self.events.put({"phase": "error", "message": str(error)})

        threading.Thread(target=play, daemon=False).start()
        self.window.after(100, self.poll)

    def begin_move(self, event):
        self.move_origin = (event.x_root, event.y_root,
                            self.window.winfo_x(), self.window.winfo_y())

    def move(self, event):
        mouse_x, mouse_y, left, top = self.move_origin
        left += event.x_root - mouse_x
        top += event.y_root - mouse_y
        left = max(0, min(left, self.window.winfo_screenwidth() - self.window.winfo_width()))
        top = max(0, min(top, self.window.winfo_screenheight() - self.window.winfo_height()))
        self.window.geometry(f"+{left}+{top}")

    def speed_ms(self):
        return max(1, round(42 / float(self.speed.get().rstrip("×"))))

    def send_playback(self):
        self.session.request("playback", paused=self.paused, speed=self.speed_ms())

    def toggle_pause(self):
        if self.ended:
            self.paused = False
            self.session.request("seek", 0, False, self.speed_ms())
            self.status.set("正在从头播放…")
            return
        self.paused = not self.paused
        self.play_button.configure(text="继续" if self.paused else "暂停")
        self.send_playback()

    def begin_drag(self, event):
        if not self.total or self.closed:
            return "break"
        self.dragging = True
        self.preview(event)

    def preview(self, event):
        if not self.dragging:
            return
        width = max(1, self.progress.winfo_width() - 18)
        frame = max(0, min(self.total, round((event.x - 9) / width * self.total)))
        self.progress.set(frame)
        self.status.set(f"{replay_time(frame)} / {replay_time(self.total)}")

    def end_drag(self, event):
        if self.dragging:
            self.preview(event)
            self.dragging = False
            self.seek()

    def keyboard_seek(self, _):
        if self.total:
            self.seek()

    def seek(self):
        frame = max(0, min(self.total, round(self.progress.get())))
        self.session.request("seek", frame, self.paused, self.speed_ms())
        self.status.set("正在定位到 " + replay_time(frame) + "…")

    def close(self):
        if self.closed:
            return
        self.session.close()
        self.close_button.configure(state="disabled")
        self.play_button.configure(state="disabled")
        self.progress.configure(state="disabled")
        self.status.set("正在关闭回放…")

    def poll(self):
        while not self.events.empty():
            state = self.events.get_nowait()
            phase = state["phase"]
            if phase in ("closed", "error"):
                self.closed = True
                self.window.destroy()
                self.finished(state.get("message"))
                return
            if phase == "loading":
                self.play_button.configure(state="disabled")
                self.status.set("正在重新载入并定位…" if state["target"] else "正在载入原版游戏录像…")
                continue
            if self.session.stopping.is_set():
                continue
            self.total = state["total"]
            self.details.set(describe(state.get("labels", self.session.labels)))
            self.current = state["frame"]
            self.ended = state["ended"]
            self.play_button.configure(state="normal")
            self.play_button.configure(text="重播" if self.ended else "继续" if self.paused else "暂停")
            self.progress.configure(state="normal", to=max(1, self.total))
            if not self.dragging:
                self.progress.set(state["target"] if state["seeking"] else self.current)
                prefix = "正在定位  " if state["seeking"] else "已播放完  " if state["ended"] else ""
                self.status.set(prefix + replay_time(self.current) + " / " + replay_time(self.total))
        self.window.after(100, self.poll)
