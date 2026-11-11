#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace CommanderRules {
struct Point { double x, y; };
inline double distance(Point a, Point b) { return std::hypot(a.x-b.x, a.y-b.y); }
struct Threat { Point point; double reach, strength; bool worker, active, detected; };
inline bool combatThreat(bool worker, bool armed) { return armed && !worker; }
inline int raidSize(int available) { return available < 8 ? 0 : available >= 12 ? 3 : 2; }
inline bool retreatComplete(int alive,int arrived) { return !alive || arrived>=std::max(1,int(std::ceil(alive*.8))); }
enum class MainAction { Delegate, AttackCombat, Move, AttackMove };
inline MainAction mainAction(bool contact, bool workerTarget, bool workerNearby,
                             bool anchored, bool waitingForExpansion) {
    if (contact) return workerTarget ? MainAction::AttackCombat : MainAction::Delegate;
    return workerTarget || workerNearby || anchored || waitingForExpansion ? MainAction::Move : MainAction::AttackMove;
}
inline bool insideDefense(Point unit, Point enemy, Point anchor, double radius) {
    return distance(unit, anchor) <= radius && distance(enemy, anchor) <= radius;
}
inline double danger(Point point, const std::vector<Threat>& threats) {
    double result = 0;
    for (const auto& t : threats) {
        if (t.worker && !t.active) continue;
        double reach = std::max(224.0, t.reach + 128.0);
        result += t.strength * std::max(0.0, (reach-distance(point,t.point))/reach)
                  * (t.detected ? 1.0 : 3.0);
    }
    return result;
}
inline double retreatCost(Point candidate, Point origin, Point preferred,
                          const std::vector<Threat>& threats) {
    return danger(candidate, threats)*2000 + distance(candidate,preferred)
           + std::abs(distance(candidate,origin)-384)*.15;
}
}
