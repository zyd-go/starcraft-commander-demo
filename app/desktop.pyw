import datetime
import os
import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

from matches import BASELINE, MAPS, OPPONENTS, ROOT, RUNS, BUILTIN, new_run, playing, read_json, store
from native_replay import ReplayPanel
from replay_info import load_labels

BG = "#101720"
PANEL = "#192432"
INK = "#f1f5f9"
MUTED = "#aebdcc"
ACCENT = "#5edbc3"
FONT = "Microsoft YaHei UI"
PREFERENCES = ROOT / "preferences.json"
LEGACY_RUNS = ROOT.parent / "human_commander_local/runs"


def history():
    for folder in sorted(RUNS.glob("*"), reverse=True):
        data = read_json(folder / "match.json")
        if data:
            yield folder, data
    for folder in sorted(LEGACY_RUNS.glob("*-user"), reverse=True):
        for replay in sorted((folder / "maps/replays").glob("*.rep"), reverse=True):
            if not replay.stat().st_size:
                continue
            yield folder, {
                "started_at": datetime.datetime.fromtimestamp(replay.stat().st_mtime).isoformat(),
                **load_labels(replay), "state": "legacy",
                "replay": replay.relative_to(folder).as_posix(),
            }


class Desktop:
    def __init__(self, window):
        self.window = window
        self.busy = False
        self.replay_panel = None
        self.active_run = None
        self.events = queue.Queue()
        self.rows = {}
        preferences = read_json(PREFERENCES) or {}
        window.title("StarCraft 指挥台")
        window.geometry("880x620")
        window.minsize(760, 565)
        window.configure(bg=BG)
        window.protocol("WM_DELETE_WINDOW", self.close)
        self.configure_style()
        self.status = tk.StringVar(value="就绪")
        self.label(window, variable=self.status, fg=MUTED, size=10).pack(
            side="bottom", anchor="w", padx=35, pady=(1, 15))

        header = tk.Frame(window, bg=BG)
        header.pack(fill="x", padx=34, pady=(26, 18))
        self.label(header, "STARCRAFT  /  本地对战", fg=ACCENT, size=10, bold=True).pack(anchor="w")
        self.label(header, "星际指挥台", size=28, bold=True).pack(anchor="w", pady=(4, 2))
        self.label(header, "选择对战配置，直接进入游戏。对局和回放保存在本机。", fg=MUTED).pack(anchor="w")

        tabs = ttk.Notebook(window)
        tabs.pack(fill="both", expand=True, padx=32, pady=(0, 10))
        self.setup = tk.Frame(tabs, bg=PANEL)
        self.archive = tk.Frame(tabs, bg=PANEL)
        tabs.add(self.setup, text="  开始对战  ")
        tabs.add(self.archive, text="  历史对局 / 回放  ")
        tabs.bind("<<NotebookTabChanged>>", lambda _: self.refresh_history())
        self.tabs = tabs
        self.map_name = tk.StringVar(value=preferences.get("map") if preferences.get("map") in MAPS else "Heartbreak Ridge")
        self.opponent_name = tk.StringVar(
            value=preferences.get("opponent") if preferences.get("opponent") in OPPONENTS else "PylonPuller")
        self.control = tk.StringVar(value=preferences.get("control", "human"))
        self.fullscreen = tk.BooleanVar(value=preferences.get("fullscreen", True))
        self.pair_notice = tk.StringVar()
        self.map_name.trace_add("write", self.update_pair_notice)
        self.opponent_name.trace_add("write", self.update_pair_notice)
        self.create_setup()
        self.create_archive()
        self.refresh_history()
        self.update_pair_notice()
        window.after(150, self.poll)

    def configure_style(self):
        style = ttk.Style(self.window)
        style.theme_use("clam")
        self.window.option_add("*TCombobox*Listbox.font", (FONT, 11))
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=BG, foreground=MUTED, padding=(13, 9), font=(FONT, 11, "bold"))
        style.map("TNotebook.Tab", background=[("selected", PANEL)], foreground=[("selected", ACCENT)])
        style.configure("TCombobox", font=(FONT, 12), padding=6, fieldbackground="#243246", foreground=INK, background="#243246")
        style.map("TCombobox", fieldbackground=[("readonly", "#243246")], foreground=[("readonly", INK)])
        style.configure("Treeview", background="#202d3c", fieldbackground="#202d3c", foreground=INK,
                        rowheight=32, font=(FONT, 10), borderwidth=0)
        style.configure("Treeview.Heading", background="#27364a", foreground=INK, font=(FONT, 10, "bold"))
        style.map("Treeview", background=[("selected", "#245b61")], foreground=[("selected", INK)])

    @staticmethod
    def label(parent, text="", fg=INK, size=11, bold=False, variable=None):
        return tk.Label(parent, text=text, textvariable=variable, fg=fg, bg=parent["bg"],
                        font=(FONT, size, "bold" if bold else "normal"), anchor="w")

    @staticmethod
    def button(parent, title, action, primary=False):
        return tk.Button(parent, text=title, command=action, relief="flat", borderwidth=0,
                         bg=ACCENT if primary else "#2a394b", fg=BG if primary else INK,
                         activebackground="#88e8d6" if primary else "#3b5067",
                         activeforeground=BG if primary else INK,
                         disabledforeground="#8396a7", font=(FONT, 11, "bold"),
                         cursor="hand2", padx=17, pady=8)

    def create_setup(self):
        area = tk.Frame(self.setup, bg=PANEL)
        area.pack(fill="both", expand=True, padx=24, pady=(20, 12))
        self.label(area, "对战配置", size=16, bold=True).pack(anchor="w")
        self.label(area, f"我方基线  {BASELINE}", fg=MUTED, size=10).pack(anchor="w", pady=(3, 10))
        form = tk.Frame(area, bg=PANEL)
        form.pack(fill="x", pady=(2, 12))
        form.columnconfigure((0, 1), weight=1, uniform="choice")
        for column, label, variable, options in (
            (0, "地图", self.map_name, tuple(MAPS)),
            (1, "对手", self.opponent_name, tuple(sorted(OPPONENTS))),
        ):
            self.label(form, label, bold=True).grid(row=0, column=column, sticky="w", padx=(0, 12), pady=(0, 5))
            ttk.Combobox(form, state="readonly", textvariable=variable, values=options,
                         font=(FONT, 12)).grid(row=1, column=column, sticky="ew", padx=(0, 12))
        mode_row = tk.Frame(area, bg=PANEL)
        mode_row.pack(fill="x", pady=(3, 2))
        for label, mode in (("八键人工指挥", "human"), ("原始基线自动对战", "baseline")):
            tk.Radiobutton(mode_row, text=label, variable=self.control, value=mode, bg=PANEL, fg=INK,
                           selectcolor="#243246", activebackground=PANEL, activeforeground=INK,
                           font=(FONT, 11), highlightthickness=0).pack(side="left", padx=(0, 14))
        tk.Checkbutton(mode_row, text="全屏", variable=self.fullscreen, bg=PANEL, fg=INK,
                       selectcolor="#243246", activebackground=PANEL, activeforeground=INK,
                       font=(FONT, 11), highlightthickness=0).pack(side="right", padx=(0, 14))
        self.label(area, "F1 侦察  F2 骚扰  F3 攻分矿  F4 就地固守", fg=MUTED, size=10).pack(anchor="w", pady=(7, 2))
        self.label(area, "F5 攻主基地  F6 保卫基地  F7 就地集结  F8 脱战后撤", fg=MUTED, size=10).pack(anchor="w", pady=(0, 8))
        self.label(area, variable=self.pair_notice, fg="#f4c577", size=10).pack(anchor="w")
        bottom = tk.Frame(area, bg=PANEL)
        bottom.pack(fill="x", side="bottom", pady=(0, 5))
        self.start_button = self.button(bottom, "开始对战  →", self.start, primary=True)
        self.start_button.pack(side="right", padx=(0, 12))
        self.label(bottom, "单局启动 · 自动存回放 · 结束后保留战绩", fg=MUTED, size=10).pack(side="left", pady=9)

    def update_pair_notice(self, *_):
        report = read_json(ROOT / "validation/compatibility.json") or {}
        key = f"{self.opponent_name.get()}|{self.map_name.get()}"
        self.pair_notice.set(report.get("notices", {}).get(key, ""))

    def create_archive(self):
        area = tk.Frame(self.archive, bg=PANEL)
        area.pack(fill="both", expand=True, padx=20, pady=16)
        actions = tk.Frame(area, bg=PANEL)
        actions.pack(side="bottom", fill="x", pady=(12, 0))
        listing = tk.Frame(area, bg=PANEL)
        listing.pack(fill="both", expand=True)
        self.tree = ttk.Treeview(listing, show="headings", columns=("date", "map", "opponent", "state", "replay"))
        for key, title, width in (
            ("date", "对局时间", 155), ("map", "地图", 125), ("opponent", "对手", 218),
            ("state", "状态", 80), ("replay", "回放", 88),
        ):
            self.tree.heading(key, text=title, anchor="w")
            self.tree.column(key, width=width, minwidth=65, anchor="w")
        scrollbar = ttk.Scrollbar(listing, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", self.selection)
        self.tree.bind("<Double-1>", self.open_replay_row)
        self.replay_button = self.button(actions, "播放 .rep", self.open_replay, primary=True)
        self.replay_button.pack(side="left")
        self.folder_button = self.button(actions, "打开对局文件夹", self.open_folder)
        self.folder_button.pack(side="left", padx=(10, 0))
        self.button(actions, "刷新", self.refresh_history).pack(side="right")

    def refresh_history(self):
        if not hasattr(self, "tree"):
            return
        selected = self.tree.selection()
        self.tree.delete(*self.tree.get_children())
        self.rows.clear()
        for folder, item in history():
            tag = f"{folder}|{item['replay']}" if item.get("state") == "legacy" else str(folder)
            replay = folder / item["replay"] if item.get("replay") else None
            if replay and not replay.is_file():
                replay = None
            state = {"finished": "已结束", "legacy": "旧对局", "closed": "已关闭", "error": "启动失败",
                     "starting": "启动中", "playing": "进行中"}.get(item.get("state"), "")
            when = item.get("started_at", "").replace("T", " ")[:16]
            self.tree.insert("", "end", iid=tag,
                             values=(when, item.get("map", ""), item.get("opponent", ""), state,
                                     ".rep" if replay else "—"))
            self.rows[tag] = (folder, replay)
        if selected and selected[0] in self.rows:
            self.tree.selection_set(selected[0])
        self.selection()

    def selected(self):
        selection = self.tree.selection()
        return self.rows.get(selection[0], (None, None)) if selection else (None, None)

    def selection(self, _=None):
        folder, replay = self.selected()
        self.folder_button.configure(state="normal" if folder else "disabled")
        self.replay_button.configure(state="normal" if replay and not self.busy else "disabled")

    def open_folder(self):
        folder, _ = self.selected()
        if folder:
            os.startfile(folder)

    def open_replay_row(self, event):
        row = self.tree.identify_row(event.y)
        if row:
            self.tree.selection_set(row)
            self.open_replay()

    def open_replay(self):
        _, replay = self.selected()
        if not replay or self.busy:
            return
        try:
            self.replay_panel = ReplayPanel(self.window, replay, self.fullscreen.get(), self.replay_finished)
            self.busy = True
            self.start_button.configure(state="disabled")
            self.selection()
            self.status.set("正在打开原版游戏回放及进度条…")
        except (OSError, ValueError, RuntimeError) as error:
            messagebox.showerror("无法打开回放", str(error), parent=self.window)

    def replay_finished(self, error):
        self.busy = False
        self.replay_panel = None
        self.start_button.configure(state="normal")
        self.selection()
        self.status.set(error or "回放已关闭。")
        if error:
            messagebox.showerror("无法播放回放", error, parent=self.window)

    def start(self):
        if self.busy:
            return
        map_name, opponent = self.map_name.get(), self.opponent_name.get()
        control, fullscreen = self.control.get(), self.fullscreen.get()
        store(PREFERENCES, dict(map=map_name, opponent=opponent, control=control, fullscreen=fullscreen))
        run = new_run()
        self.active_run = run
        self.busy = True
        self.start_button.configure(state="disabled")
        self.selection()
        self.status.set(f"正在准备 {map_name} / {opponent}…")

        def run_match():
            try:
                result = playing(run, map_name, opponent, control, fullscreen=fullscreen,
                                 update=lambda value: self.events.put(("status", value)))
                self.events.put(("done", result))
            except Exception as error:
                self.events.put(("error", str(error)))

        threading.Thread(target=run_match, daemon=False).start()

    def poll(self):
        while not self.events.empty():
            event, value = self.events.get_nowait()
            if event == "status":
                self.status.set(value)
                continue
            self.busy = False
            self.active_run = None
            self.start_button.configure(state="normal")
            self.refresh_history()
            if event == "done":
                self.tabs.select(self.archive)
                self.status.set("本局已结束，.rep 录像已保存。" if value.get("replay")
                                else "本局已结束，本次未生成 .rep 录像。")
                if self.tree.get_children():
                    self.tree.selection_set(self.tree.get_children()[0])
            else:
                self.status.set(value)
                messagebox.showerror("对局未能正常完成", value, parent=self.window)
        self.window.after(150, self.poll)

    def close(self):
        if self.replay_panel is not None:
            self.replay_panel.close()
            return
        if self.busy:
            match = read_json(self.active_run / "match.json") if self.active_run else None
            if match and match.get("results_open"):
                messagebox.showinfo("战绩统计正在显示", "回放已经保存。看完战绩后，点击游戏中的 OK 即会关闭游戏并返回指挥台；也可按 Alt+F4。", parent=self.window)
            else:
                messagebox.showinfo("对局正在运行", "请先在游戏中结束本局。软件会保存回放，并保留战绩页面供你查看。", parent=self.window)
            return
        self.window.destroy()


if __name__ == "__main__":
    root = tk.Tk()
    Desktop(root)
    root.mainloop()
