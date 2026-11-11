#pragma once
#include <BWAPI.h>
#include <fstream>
#include <string>

namespace MatchIdentity {
inline std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (unsigned char ch : value) {
        if (ch == '\\' || ch == '"') { result += '\\'; result += char(ch); }
        else if (ch < 32 || ch >= 127) {
            const char* hex = "0123456789abcdef";
            result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15];
        } else result += char(ch);
    }
    return result + "\"";
}

inline void save(BWAPI::Game* game) {
    std::ofstream out("bwapi-data/write/match-identity.json", std::ios::trunc);
    out << "{\"map_actual\":" << quoted(game->mapName()) << ",\"self_player_id\":"
        << (game->self() ? game->self()->getID() : -1) << ",\"players\":[";
    bool first = true;
    for (auto player : game->getPlayers()) {
        auto type = player->getType();
        if (player->isNeutral() || player->isObserver() ||
            (type != BWAPI::PlayerTypes::Player && type != BWAPI::PlayerTypes::Computer &&
             type != BWAPI::PlayerTypes::PlayerLeft && type != BWAPI::PlayerTypes::ComputerLeft)) continue;
        if (!first) out << ',';
        first = false;
        auto color = player->getColor();
        out << "{\"id\":" << player->getID() << ",\"name\":" << quoted(player->getName())
            << ",\"race\":" << quoted(player->getRace().getName())
            << ",\"color_id\":" << color.getID()
            << ",\"text_color\":" << int(static_cast<unsigned char>(player->getTextColor()))
            << ",\"color_rgb\":["
            << color.red() << ',' << color.green() << ',' << color.blue() << "]}";
    }
    out << "]}\n";
}
}
