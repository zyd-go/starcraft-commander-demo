"""Recorded match labels and the actual player colors read by native BWAPI."""
import configparser
from pathlib import Path
import re

from matches import BUILTIN, read_json, store

COLOR_NAMES = {111: "红色", 165: "蓝色", 159: "青绿色", 164: "紫色", 156: "橙色",
               19: "棕色", 255: "白色", 135: "黄色", 117: "绿色", 128: "青色"}
TEXT_COLORS = {8: "红色", 14: "蓝色", 15: "青绿色", 16: "紫色", 17: "橙色",
               21: "棕色", 22: "白色", 23: "黄色", 24: "绿色", 26: "青色"}


def map_label(value):
    name = Path(str(value).replace("\\", "/")).stem
    return re.sub(r"^\(\d+\)\s*", "", name) or "未记录"


def load_labels(replay):
    replay = Path(replay)
    sidecar = read_json(replay.with_suffix(".rep.json"))
    if sidecar:
        return sidecar
    for folder in replay.parents:
        recorded = read_json(folder / "match.json") or read_json(folder / "manifest.json")
        if not recorded:
            continue
        data = {key: recorded[key] for key in ("map", "opponent", "control", "started_at",
                "self_player_id", "players", "map_actual", "our_player_name") if key in recorded}
        data["map"] = map_label(data.get("map", ""))
        if data.get("opponent", "").startswith("Original Computer"):
            data["opponent"] = BUILTIN
        # Read the original launch configuration, rather than assuming our slot/color.
        config = configparser.ConfigParser(interpolation=None)
        try:
            config.read(folder / "bwapi-data/bwapi.ini", encoding="utf-8-sig")
            data.setdefault("our_player_name", config.get("auto_menu", "character_name"))
        except (OSError, UnicodeError, configparser.Error):
            pass
        return data
    return {}


def identify_ours(labels, players):
    player_id = labels.get("self_player_id")
    if player_id is not None and player_id >= 0:
        return next((p for p in players if p["id"] == player_id), None)
    name = labels.get("our_player_name")
    matches = [p for p in players if name and p["name"] == name]
    return matches[0] if len(matches) == 1 else None


def describe(labels):
    players = labels.get("players", [])
    ours = identify_ours(labels, players)
    enemies = [p for p in players if ours and p["id"] != ours["id"]]
    enemy = enemies[0] if len(enemies) == 1 else None
    def color_name(player):
        if not player:
            return "待识别"
        return COLOR_NAMES.get(player.get("color_id"), TEXT_COLORS.get(
            player.get("text_color"), f"色号 {player.get('color_id')}"))
    # The scenario's internal name can be localized/formatted; launch records
    # preserve the friendly name of the exact map the user selected.
    name = map_label(labels.get("map") or labels.get("map_actual", ""))
    opponent = labels.get("opponent") or "未记录"
    return f"我方：{color_name(ours)}  |  对手：{opponent}（{color_name(enemy)}）  |  地图：{name}"


def save_match_labels(run, status, replay=None):
    identity = read_json(run / "bwapi-data/write/match-identity.json") or {}
    status.update(identity)
    ours = identify_ours(status, status.get("players", []))
    if ours:
        status["our_player_name"] = ours["name"]
    if replay:
        store(replay.with_suffix(".rep.json"), {key: status[key] for key in (
            "map", "opponent", "control", "started_at", "self_player_id", "players",
            "map_actual", "our_player_name") if key in status})
