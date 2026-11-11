// Human-command adapter. Baseline.dll is loaded unchanged and owns the economy.
#include <windows.h>
#include <BWAPI.h>
#include <bwem.h>
#include "MatchIdentity.h"
#include "CommanderRules.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
using namespace BWAPI;
using namespace UnitTypes;
namespace Rules = CommanderRules;
static Game* game = nullptr;
static std::vector<int> keys;
static ULONGLONG lastInput = 0;
static LPARAM mousePoint = -1;

static LRESULT CALLBACK input(int code, WPARAM removal, LPARAM data) {
    if (code == HC_ACTION && removal == PM_REMOVE) {
        MSG& m = *reinterpret_cast<MSG*>(data);
        if ((m.message == WM_MOUSEMOVE && m.lParam != mousePoint) ||
            (m.message >= WM_LBUTTONDOWN && m.message <= WM_MOUSEHWHEEL) || m.message == WM_KEYDOWN)
            lastInput = GetTickCount64();
        if (m.message == WM_MOUSEMOVE) mousePoint = m.lParam;
        if ((m.message == WM_KEYDOWN || m.message == WM_KEYUP) && m.wParam >= VK_F1 && m.wParam <= VK_F8) {
            if (m.message == WM_KEYDOWN && !(m.lParam & (1L << 30))) keys.push_back(int(m.wParam));
            // Consume both edges, including repeat messages: F1-F4 must not open
            // native help/camera shortcuts. F9 is unbound; F10 remains the menu.
            m.message = WM_NULL; m.wParam = 0; m.lParam = 0;
        }
    }
    return CallNextHookEx(nullptr, code, removal, data);
}
static std::string env(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name); return value ? value : fallback;
}
static BOOL CALLBACK findGameWindow(HWND window, LPARAM result) {
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    *reinterpret_cast<HWND*>(result) = window; return FALSE;
}

class CommanderLayer final : public AIModule {
    struct Memory { Position position; UnitType type; };
    struct Guard { int worker, since; Position origin; };
    HMODULE library = nullptr;
    AIModule* delegate = nullptr;
    HHOOK hook = nullptr;
    bool human = false, ended = false, scoutRequested = false, scoutReturning = false, testStop = false;
    bool surveyExpansion = false, harassReturning = false;
    int frame = -1, lastTick = -1, lastCamera = -1, smokeLimit = 0;
    int scoutId = -1, scoutSince = 0, scoutRetry = 0, harassSince = 0, lastRetreatPlan = -1000;
    int baitOverrides = 0, guardPeak = 0, lastTestSequence = 0;
    std::string stance = "rally", feedback = "ready", role;
    std::string scoutStatus = "idle", harassStatus = "idle", testCommands;
    Position home, enemyStart, rally, holdPoint, gatherPoint, fallback = Positions::None;
    Position retreatOrigin, scoutTarget = Positions::None, harassTarget = Positions::None;
    TilePosition searchSite = TilePositions::Invalid;
    std::map<int,Memory> remembered;
    std::map<int,int> lastOrder, workerIgnoredUntil;
    std::map<int,Guard> guards;
    std::map<std::pair<int,int>,int> visited;
    std::set<int> harassIds, retreatIds;
    std::vector<Unit> army, foes, bases;
    std::ofstream log;

    Rules::Point point(Position p) const { return {double(p.x),double(p.y)}; }
    Position clamp(Position p) const {
        return Position(std::clamp(p.x,16,game->mapWidth()*32-17),std::clamp(p.y,16,game->mapHeight()*32-17));
    }
    Position toward(Position a, Position b, double distance) const {
        double d = a.getDistance(b);
        return d < 1 ? a : clamp(Position(int(a.x+(b.x-a.x)*distance/d),int(a.y+(b.y-a.y)*distance/d)));
    }
    bool live(Unit u) const { return u && u->exists() && u->getHitPoints() > 0; }
    Unit ours(int id) const { Unit u = id < 0 ? nullptr : game->getUnit(id); return live(u) && u->getPlayer()==game->self() ? u : nullptr; }
    Unit enemy(int id) const {
        for (auto u : foes) if (u->getID()==id) return u;
        return nullptr;
    }
    bool auxiliary(Unit u) const { return u->getID()==scoutId || harassIds.count(u->getID()) || guards.count(u->getID()); }
    std::vector<Unit> mainArmy() const {
        std::vector<Unit> result;
        for (auto u : army) if (!auxiliary(u)) result.push_back(u);
        return result;
    }
    Position center() const {
        auto units = mainArmy();
        if (units.empty()) return home;
        Unit core = units.front(); size_t count = 0;
        for (auto a : units) {
            size_t nearby = std::count_if(units.begin(),units.end(),[&](Unit b){return a->getDistance(b)<384;});
            if (nearby > count) { count=nearby; core=a; }
        }
        Position sum(0,0); int n=0;
        for (auto u : units) if (u->getDistance(core)<384) { sum += u->getPosition(); ++n; }
        return Position(sum.x/std::max(1,n),sum.y/std::max(1,n));
    }
    bool groundPoint(Position p, const std::vector<Unit>& units) const {
        if (!p.isValid() || !game->isWalkable(WalkPosition(p))) return false;
        return std::all_of(units.begin(),units.end(),[&](Unit u){return u->isFlying() || u->hasPath(p);});
    }
    Position localPoint(Position desired) const {
        auto units = mainArmy(); desired=clamp(desired);
        if (groundPoint(desired,units)) return desired;
        for (int r=32;r<=192;r+=32) for (int i=0;i<16;++i) {
            double a=i*3.141592653589793/8;
            Position p=clamp(desired+Position(int(std::cos(a)*r),int(std::sin(a)*r)));
            if (groundPoint(p,units)) return p;
        }
        return units.empty()?home:units.front()->getPosition();
    }
    double health(Unit u) const {
        auto t=u->getType(); return double(u->getHitPoints()+u->getShields())/std::max(1,t.maxHitPoints()+t.maxShields());
    }
    bool armed(Unit u) const { return u->getType().canAttack() || u->getType()==Protoss_Reaver; }
    bool realThreat(Unit u) const { return Rules::combatThreat(u->getType().isWorker(),armed(u)); }
    double strength(Unit u) const {
        if (!u->isCompleted() && !u->getType().isWorker()) return .2;
        double value=u->getType().isWorker()?.3:u->getType()==Protoss_Reaver?7:
                     u->getType().isBuilding()?4:u->getType()==Protoss_Dragoon?3:2;
        return value*std::max(.2,health(u));
    }
    std::vector<Rules::Threat> threats() const {
        std::vector<Rules::Threat> result;
        for (auto u : foes) if (armed(u) && !u->isFlying()) {
            int reach=u->getType().groundWeapon().maxRange();
            result.push_back({point(u->getPosition()),double(reach),strength(u),u->getType().isWorker(),
                              u->isAttacking() || u->isConstructing(),u->isDetected()});
        }
        return result;
    }
    void refresh() {
        army.clear(); foes.clear(); bases.clear();
        for (auto u : game->self()->getUnits()) if (live(u) && u->isCompleted()) {
            if (u->getType().isResourceDepot()) bases.push_back(u);
            if (!u->getType().isWorker() && !u->getType().isBuilding() && armed(u)) army.push_back(u);
        }
        for (auto player : game->enemies()) for (auto u : player->getUnits()) if (live(u) && u->isVisible()) {
            foes.push_back(u);
            if (u->getType().isBuilding()) remembered[u->getID()]={u->getPosition(),u->getType()};
        }
        for (auto it=remembered.begin();it!=remembered.end();) {
            if (game->isVisible(TilePosition(it->second.position)) && !enemy(it->first)) it=remembered.erase(it);
            else ++it;
        }
        for (auto it=harassIds.begin();it!=harassIds.end();) {
            if (!ours(*it)) it=harassIds.erase(it); else ++it;
        }
    }
    bool issueMove(Unit u, Position destination, bool attackMove=false) {
        if (!live(u) || !destination.isValid() || frame-lastOrder[u->getID()]<8) return false;
        if (u->isAttackFrame() || u->isStartingAttack()) return false;
        const auto desired=attackMove?Orders::AttackMove:Orders::Move;
        if (u->getOrder()==desired && !u->isIdle() && !u->isStuck() && u->getTargetPosition().getDistance(destination)<48) return false;
        bool ok=attackMove?u->attack(destination):u->move(destination);
        if (ok) lastOrder[u->getID()]=frame;
        return ok;
    }
    bool issueAttack(Unit u, Unit target) {
        if (!live(u) || !live(target) || !target->isDetected() || !u->canAttack(target) || frame-lastOrder[u->getID()]<8) return false;
        if (u->isAttackFrame() || u->isStartingAttack()) return false;
        if (u->getOrderTarget()==target && !u->isIdle()) return false;
        bool ok=u->attack(target); if (ok) lastOrder[u->getID()]=frame; return ok;
    }
    Position defendedBase() const {
        Position result=home; double best=-1e18;
        for (auto base : bases) {
            double score=-base->getDistance(enemyStart)*.001;
            for (auto e : foes) if (armed(e) && (!e->getType().isWorker() || e->isAttacking() || e->isConstructing()))
                score += strength(e)*std::max(0.0,640.0-e->getDistance(base))*10;
            if (score>best) {best=score;result=base->getPosition();}
        }
        return result;
    }
    Position expansionGoal() const {
        Position result=Positions::None; double best=1e18;
        for (const auto& entry : remembered) if (entry.second.type.isResourceDepot() && entry.second.position.getDistance(enemyStart)>640) {
            double d=entry.second.position.getDistance(center());
            if (d<best) {best=d;result=entry.second.position;}
        }
        return result;
    }
    Position scoutPoint(bool expansions) {
        if (!expansions && !game->isExplored(TilePosition(enemyStart))) return enemyStart;
        Position result=Positions::None; double best=-1e18;
        Position origin=ours(scoutId)?ours(scoutId)->getPosition():center();
        for (const auto& area : BWEM::Map::Instance().Areas()) for (const auto& base : area.Bases()) {
            Position p=Position(base.Location())+Position(64,48);
            if (std::any_of(bases.begin(),bases.end(),[&](Unit b){return b->getDistance(p)<400;})) continue;
            if (expansions && p.getDistance(enemyStart)<640) continue;
            auto key=std::make_pair(base.Location().x,base.Location().y);
            double age=visited.count(key)?frame-visited[key]:10000;
            double score=age-origin.getDistance(p)*.5+(game->isExplored(base.Location())?0:4000);
            if (score>best) {best=score;result=p;}
        }
        return result==Positions::None?enemyStart:result;
    }
    Position mainGoal() {
        if (stance=="attack") {
            Position target=enemyStart; double best=1e18;
            for (const auto& e : remembered) if (e.second.position.getDistance(enemyStart)<800) {
                double d=e.second.position.getDistance(enemyStart);
                if (d<best) {best=d;target=e.second.position;}
            }
            if (best<1e18 || !game->isExplored(TilePosition(enemyStart))) return target;
            if (!remembered.empty()) return remembered.begin()->second.position;
            return scoutPoint(false);
        }
        if (stance=="expansion") {
            Position p=expansionGoal(); return p==Positions::None?gatherPoint:p;
        }
        if (stance=="defend") return localPoint(toward(defendedBase(),enemyStart,160));
        if (stance=="hold") return holdPoint;
        if (stance=="retreat") return fallback;
        return gatherPoint;
    }
    bool unseenDanger(Unit u) const {
        return std::any_of(foes.begin(),foes.end(),[&](Unit e){return realThreat(e) && !e->isDetected() && !e->isFlying() && u->getDistance(e)<256;});
    }
    Position detectionPoint(Unit u) const {
        Position best=Positions::None; double distance=1e18;
        for (auto a : game->self()->getUnits()) if (live(a) && a->isCompleted() &&
            (a->getType()==Protoss_Observer || (a->getType()==Protoss_Photon_Cannon && a->isPowered())) && u->hasPath(a->getPosition())) {
            double d=u->getDistance(a); if (d<distance) {distance=d;best=a->getPosition();}
        }
        return best;
    }
    Position retreatPoint() {
        Position origin=center(), danger=enemyStart; double weight=0; Position sum(0,0);
        for (auto e : foes) if (realThreat(e) && e->getDistance(origin)<800) {
            int w=std::max(1,int(strength(e)*10)); sum+=Position(e->getPosition().x*w,e->getPosition().y*w); weight+=w;
        }
        if (weight) danger=Position(int(sum.x/weight),int(sum.y/weight));
        Position preferred=toward(origin,danger,-384), best=Positions::None;
        auto units=mainArmy(); auto risks=threats(); double bestScore=1e18;
        auto consider=[&](Position p) {
            p=clamp(p); double d=p.getDistance(origin);
            if (d<160 || d>640 || !groundPoint(p,units)) return;
            double score=Rules::retreatCost(point(p),point(origin),point(preferred),risks);
            bool cloaked=std::any_of(foes.begin(),foes.end(),[&](Unit e){return realThreat(e) && !e->isDetected() && e->getDistance(origin)<600;});
            if (cloaked) for (auto a : game->self()->getUnits()) if (live(a) && a->isCompleted() &&
                (a->getType()==Protoss_Observer || (a->getType()==Protoss_Photon_Cannon && a->isPowered())) && a->getDistance(p)<224) score-=600;
            if (score<bestScore) {bestScore=score;best=p;}
        };
        consider(preferred);
        for (int radius : {256,384,512}) for (int i=0;i<16;++i) {
            double a=i*3.141592653589793/8;
            consider(origin+Position(int(std::cos(a)*radius),int(std::sin(a)*radius)));
        }
        return best==Positions::None?localPoint(toward(origin,home,std::min(256.0,origin.getDistance(home)))):best;
    }
    void event(const char* name, const std::string& reason) {
        log<<"{\"event\":\""<<name<<"\",\"frame\":"<<frame<<",\"reason\":\""<<reason<<"\"}"<<std::endl;
    }
    void startScout(bool survey) {
        bool changed=survey && !surveyExpansion;
        scoutRequested=true; surveyExpansion=surveyExpansion || survey;
        if(changed && ours(scoutId))scoutTarget=scoutPoint(true);
        if (!ours(scoutId)) scoutStatus="waiting_units";
        else scoutStatus="active";
    }
    void updateScout() {
        Unit scout=ours(scoutId);
        if (scoutId>=0 && !scout) {scoutId=-1;scoutRequested=false;scoutStatus="lost";scoutRetry=frame+240;}
        if (!scout && scoutRequested && frame>=scoutRetry) {
            int workers=0;
            for (auto u : game->self()->getUnits()) if (u->getType().isWorker() && u->isCompleted()) ++workers;
            double best=1e18;
            for (auto u : game->self()->getUnits()) if (workers>=8 && live(u) && u->isCompleted() && u->getType().isWorker() &&
                !u->isConstructing() && u->getBuildType()==UnitTypes::None && !u->isCarryingMinerals() && !u->isCarryingGas() && health(u)>.65) {
                double score=u->getDistance(enemyStart);
                if (score<best) {best=score;scout=u;}
            }
            if (!scout && mainArmy().size()>=4) for (auto u : mainArmy()) if (u->getType()!=Protoss_Reaver && health(u)>.7) {scout=u;break;}
            if (scout) {
                scoutId=scout->getID();scoutSince=frame;scoutReturning=false;scoutTarget=scoutPoint(surveyExpansion);
                scoutStatus="active";event("scout_start",std::to_string(scoutId));
            } else scoutStatus="waiting_units";
        }
        if (!scout) return;
        if (scout->isConstructing() || scout->getBuildType()!=UnitTypes::None) {
            scoutId=-1;scoutStatus="builder_released";scoutRetry=frame+240;return;
        }
        Unit danger=nullptr;
        for (auto e : foes) if (realThreat(e) && scout->getDistance(e)<(scout->isFlying()?256:224) && (!danger || scout->getDistance(e)<scout->getDistance(danger))) danger=e;
        if (danger || health(scout)<.45 || frame-scoutSince>1440) scoutReturning=true;
        if (scoutReturning) {
            scoutStatus="returning";
            if (scout->getDistance(home)<180) {scoutId=-1;scoutRequested=false;surveyExpansion=false;scoutStatus="idle";scoutRetry=frame+720;return;}
            issueMove(scout,danger?toward(scout->getPosition(),danger->getPosition(),-224):home);
            return;
        }
        if (scoutTarget==Positions::None || scout->getDistance(scoutTarget)<96) {
            if (scoutTarget!=Positions::None) {auto t=TilePosition(scoutTarget);visited[{t.x-2,t.y-1}]=frame;}
            scoutTarget=scoutPoint(surveyExpansion);
        }
        if (!scout->hasPath(scoutTarget)) {scoutReturning=true;scoutStatus="blocked";return;}
        issueMove(scout,scoutTarget);
    }
    void startHarass() {
        if (!harassIds.empty()) {feedback="harass_already_active";return;}
        auto candidates=mainArmy(); int wanted=Rules::raidSize(int(candidates.size()));
        if (!wanted) {harassStatus="not_enough_army";feedback=harassStatus;return;}
        candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](Unit u){return health(u)<.7 || u->getType()==Protoss_Reaver;}),candidates.end());
        if (int(candidates.size())<wanted+6) {harassStatus="not_enough_healthy_units";feedback=harassStatus;return;}
        Position destination=expansionGoal(); if (destination==Positions::None) destination=enemyStart;
        std::sort(candidates.begin(),candidates.end(),[&](Unit a,Unit b){return a->getDistance(destination)<b->getDistance(destination);});
        for (int i=0;i<wanted;++i) harassIds.insert(candidates[i]->getID());
        harassTarget=destination;harassSince=frame;harassReturning=false;harassStatus="active";
        event("harass_start",std::to_string(wanted));
    }
    void updateHarass() {
        if (harassIds.empty()) {if (harassStatus=="active" || harassStatus=="returning") harassStatus="idle";return;}
        double ownPower=0,enemyPower=0; Position location(0,0);
        for (int id : harassIds) {auto u=ours(id);if (u) {location+=u->getPosition();ownPower+=strength(u);if(health(u)<.4)harassReturning=true;}}
        location=Position(location.x/int(harassIds.size()),location.y/int(harassIds.size()));
        for (auto e : foes) if (realThreat(e) && e->getDistance(location)<400) enemyPower+=strength(e);
        if (enemyPower>ownPower*.9 || frame-harassSince>1440) harassReturning=true;
        Position back=center();
        std::vector<int> finished;
        for (int id : harassIds) if (auto u=ours(id)) {
            if (harassReturning || unseenDanger(u)) {
                harassStatus="returning";
                if (u->getDistance(back)<160) {finished.push_back(id);continue;}
                issueMove(u,back);continue;
            }
            Unit target=nullptr;double best=1e18;
            for (auto e : foes) if (e->getType().isWorker() && e->isDetected() && u->canAttack(e) && e->getDistance(harassTarget)<352 && u->getDistance(e)<320) {
                double d=u->getDistance(e);if(d<best){best=d;target=e;}
            }
            if (target) issueAttack(u,target); else issueMove(u,harassTarget);
        }
        for (int id : finished) harassIds.erase(id);
    }
    bool dangerousWorker(Unit e) const {
        return e->getType().isWorker() && (e->isAttacking() || e->isConstructing()) &&
            std::any_of(bases.begin(),bases.end(),[&](Unit b){return e->getDistance(b)<352;});
    }
    void updateGuards() {
        for (auto it=guards.begin();it!=guards.end();) {
            Unit u=ours(it->first),w=enemy(it->second.worker);
            if (!u || !w || !dangerousWorker(w) || frame-it->second.since>72 || u->getDistance(it->second.origin)>192 || stance=="retreat") {
                if(w)workerIgnoredUntil[w->getID()]=frame+120;
                it=guards.erase(it);
            } else ++it;
        }
        if (stance=="retreat") return;
        const size_t limit=army.size()>=8?2:1;
        for (auto w : foes) if (dangerousWorker(w) && workerIgnoredUntil[w->getID()]<=frame) {
            if (std::any_of(guards.begin(),guards.end(),[&](const auto& g){return g.second.worker==w->getID();})) continue;
            if (guards.size()>=limit) break;
            Unit best=nullptr;
            for (auto u : mainArmy()) if (u->canAttack(w) && u->getDistance(w)<320 &&
                (!best || u->getDistance(w)<best->getDistance(w))) best=u;
            if (best) guards[best->getID()]={w->getID(),frame,best->getPosition()};
        }
        guardPeak=std::max(guardPeak,int(guards.size()));
        for (const auto& g : guards) issueAttack(ours(g.first),enemy(g.second.worker));
    }
    Unit localCombat(Unit u, Position anchor, double radius, bool immediate) const {
        Unit result=nullptr;double nearest=1e18;
        for (auto e : foes) if (!e->getType().isWorker() && e->isDetected() && u->canAttack(e)) {
            if (radius>0 && !Rules::insideDefense(point(u->getPosition()),point(e->getPosition()),point(anchor),radius)) continue;
            double range=immediate?game->self()->weaponMaxRange(u->getType().groundWeapon())+48:400;
            if (e->getType().isBuilding() && !armed(e) && stance!="attack" && stance!="expansion") continue;
            double d=u->getDistance(e);if(d<range && d<nearest){nearest=d;result=e;}
        }
        return result;
    }
    bool workerNearby(Unit u) const {
        return std::any_of(foes.begin(),foes.end(),[&](Unit e){return e->getType().isWorker() && u->getDistance(e)<432;});
    }
    void control() {
        if (stance=="expansion" && expansionGoal()==Positions::None && frame>=scoutRetry) {
            startScout(true);feedback="expansion_unknown_scouting";
        }
        if(stance=="expansion" && expansionGoal()!=Positions::None)feedback="attacking_known_expansion";
        updateScout();updateHarass();updateGuards();
        if (stance=="retreat") {
            auto risks=threats();
            if (frame-lastRetreatPlan>72 && Rules::danger(point(fallback),risks)>Rules::danger(point(center()),risks)+.5) {
                fallback=retreatPoint();lastRetreatPlan=frame;event("retreat_replan","destination_threatened");
            }
            int alive=0,arrived=0;
            for (int id : retreatIds) if(auto u=ours(id)){++alive;if(u->getDistance(fallback)<160)++arrived;}
            if (Rules::retreatComplete(alive,arrived)) {
                gatherPoint=fallback;stance="rally";feedback="retreat_complete_regroup";event("retreat_complete","safe_regroup");
            }
        }
        Position target=mainGoal();int slot=0;
        for (auto u : mainArmy()) {
            if (stance=="retreat") {issueMove(u,target);continue;}
            if (unseenDanger(u)) {
                Position safe=detectionPoint(u);
                if (safe!=Positions::None) issueMove(u,safe);
                // Preserve the delegate's undetected-threat avoidance when no detector exists.
                continue;
            }
            double radius=stance=="hold"?256:stance=="defend"?480:stance=="rally"?192:0;
            bool outside=radius>0 && u->getDistance(target)>radius;
            Unit combat=outside?nullptr:localCombat(u,target,radius,stance=="rally");
            Unit current=u->getOrderTarget();
            auto command=u->getLastCommand();
            if(command.getType()==UnitCommandTypes::Attack_Unit && command.getTarget())current=command.getTarget();
            bool workerTarget=current && current->getType().isWorker();
            bool bait=workerNearby(u) || workerTarget;
            auto action=Rules::mainAction(combat!=nullptr,workerTarget,bait,radius>0,
                                         stance=="expansion" && expansionGoal()==Positions::None);
            if (action==Rules::MainAction::Delegate || action==Rules::MainAction::AttackCombat) {
                if (action==Rules::MainAction::AttackCombat) {issueAttack(u,combat);++baitOverrides;}
                else if (!current && u->isIdle()) issueAttack(u,combat);
                continue;
            }
            Position destination=target;
            if (stance=="rally" || stance=="hold") {
                destination=localPoint(target+Position((slot%5-2)*32,(slot/5%5-2)*32));++slot;
            }
            if (issueMove(u,destination,action==Rules::MainAction::AttackMove) && bait) ++baitOverrides;
        }
        Position escort=center();
        for (auto u : game->self()->getUnits()) if (live(u) && u->isCompleted() && u->getType()==Protoss_Observer && u->getID()!=scoutId) {
            bool danger=std::any_of(foes.begin(),foes.end(),[&](Unit e){return e->getType().airWeapon()!=WeaponTypes::None && e->getDistance(u)<256;});
            if (!danger) issueMove(u,toward(escort,home,64));
        }
    }
    void applyKeys() {
        for (int key : keys) {
            feedback="applied";
            switch (key) {
                case VK_F1: startScout(false);break;
                case VK_F2: startHarass();break;
                case VK_F3: stance="expansion";gatherPoint=localPoint(center());if(expansionGoal()==Positions::None)startScout(true);break;
                case VK_F4: stance="hold";holdPoint=localPoint(center());break;
                case VK_F5: stance="attack";break;
                case VK_F6: stance="defend";break;
                case VK_F7: stance="rally";gatherPoint=localPoint(center());break;
                case VK_F8: {
                    auto cohort=mainArmy();
                    scoutRequested=false;scoutReturning=true;if(ours(scoutId))scoutStatus="returning";
                    harassIds.clear();harassStatus="recalled";guards.clear();
                    retreatOrigin=center();fallback=retreatPoint();lastRetreatPlan=frame;
                    if(cohort.empty())cohort=mainArmy();
                    retreatIds.clear();for(auto u:cohort)retreatIds.insert(u->getID());stance="retreat";break;
                }
            }
            Position target=mainGoal(),base=defendedBase();
            log<<"{\"event\":\"command\",\"frame\":"<<frame<<",\"key\":\"F"<<(key-VK_F1+1)
               <<"\",\"stance\":\""<<stance<<"\",\"status\":\""<<feedback<<"\",\"goal\":["<<target.x<<","<<target.y
               <<"],\"origin\":["<<center().x<<","<<center().y<<"],\"defend_base\":["<<base.x<<","<<base.y
               <<"],\"retreat_units\":"<<retreatIds.size()<<"}"<<std::endl;
        }
        keys.clear();
    }
    void testInput() {
        if (testCommands.empty()) return;
        std::ifstream file(testCommands);int sequence=0,key=0;
        if (file>>sequence>>key && sequence>lastTestSequence && key>=0 && key<=8) {
            lastTestSequence=sequence;
            if(key==0){testStop=true;game->leaveGame();}else keys.push_back(VK_F1+key-1);
        }
    }
    const char* mainLabel() const {
        if(stance=="attack")return "Attack main base";
        if(stance=="expansion")return "Attack expansion";
        if(stance=="defend")return "Defend base";
        if(stance=="hold")return "Hold this area";
        if(stance=="retreat")return "Disengage";
        return "Regroup here";
    }
    const char* scoutLabel() const {
        if(scoutStatus=="active")return "Scouting";
        if(scoutStatus=="returning" || scoutStatus=="blocked")return "Returning";
        if(scoutStatus=="waiting_units" || scoutStatus=="builder_released")return "Waiting for unit";
        if(scoutStatus=="lost")return "Scout lost";
        return "Idle";
    }
    const char* raidLabel() const {
        if(harassStatus=="active")return "Raiding";
        if(harassStatus=="returning")return "Returning";
        if(harassStatus=="not_enough_army" || harassStatus=="not_enough_healthy_units")return "Need 8 healthy troops";
        return "Idle";
    }
    const char* feedbackLabel() const {
        if(feedback=="expansion_unknown_scouting")return "No enemy expansion found: scouting first";
        if(feedback=="attacking_known_expansion")return "Advancing to discovered enemy expansion";
        if(feedback=="retreat_complete_regroup")return "Safe point reached: regrouping";
        if(feedback=="not_enough_army" || feedback=="not_enough_healthy_units")return "Main force preserved: need more healthy troops for a raid";
        return "Command active";
    }
public:
    void onStart() override {
        role=env("COMMANDER_ROLE","human");human=role=="human" && env("COMMANDER_CONTROL","human")=="human";
        smokeLimit=std::atoi(env("COMMANDER_SMOKE_FRAMES","0").c_str());
        testCommands=env("COMMANDER_TEST_COMMANDS");
        if(role=="human" && env("COMMANDER_RESULTS_WATCH")=="1") {
            const auto config=env("BWAPI_CONFIG_INI");
            if(!config.empty())WritePrivateProfileStringA("auto_menu","auto_menu","OFF",config.c_str());
        }
        log.open("bwapi-data/write/commander-layer.jsonl");
        library=LoadLibraryA(env("COMMANDER_DELEGATE","bwapi-data/AI/Baseline.dll").c_str());
        auto init=library?reinterpret_cast<void(*)(Game*)>(GetProcAddress(library,"gameInit")):nullptr;
        auto create=library?reinterpret_cast<AIModule*(*)()>(GetProcAddress(library,"newAIModule")):nullptr;
        if (!init || !create) {event("load_error",std::to_string(GetLastError()));game->leaveGame();return;}
        init(game);delegate=create();delegate->onStart();MatchIdentity::save(game);
        home=Position(game->self()->getStartLocation())+Position(64,48);enemyStart=home;
        for(auto t:game->getStartLocations())if(t!=game->self()->getStartLocation())enemyStart=Position(t)+Position(64,48);
        rally=toward(home,enemyStart,220);gatherPoint=rally;holdPoint=rally;
        if (human) {
            BWEM::Map::Instance().Initialize(game);BWEM::Map::Instance().FindBasesForStartingLocations();
            keys.clear();lastInput=GetTickCount64();mousePoint=-1;
            hook=SetWindowsHookExW(WH_GETMESSAGE,input,nullptr,GetCurrentThreadId());
        }
        log<<"{\"event\":\"start\",\"role\":\""<<role<<"\",\"human_layer\":"<<(human?"true":"false")
           <<",\"delegate_loaded\":true,\"controls\":\"eight-key-v1\"}"<<std::endl;
        HWND window=nullptr;EnumWindows(findGameWindow,reinterpret_cast<LPARAM>(&window));
        std::ofstream("bwapi-data/write/game-window.txt")<<reinterpret_cast<uintptr_t>(window);
    }
    void onFrame() override {
        if (!delegate || ended) return;
        frame=game->getFrameCount();
        if(smokeLimit>0 && frame>=smokeLimit){game->leaveGame();return;}
        if(lastTick==frame)return;lastTick=frame;
        delegate->onFrame();
        if(frame%8==0)refresh();
        if(human) {
            testInput();if(testStop)return;bool commandPending=!keys.empty();
            if(commandPending)refresh();applyKeys();
            if(frame%8==0 || commandPending)control();
            DWORD foreground=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
            if(foreground==GetCurrentProcessId() && (GetAsyncKeyState(VK_LBUTTON)&0x8000 || GetAsyncKeyState(VK_RBUTTON)&0x8000 ||
                GetAsyncKeyState(VK_LEFT)&0x8000 || GetAsyncKeyState(VK_RIGHT)&0x8000 || GetAsyncKeyState(VK_UP)&0x8000 || GetAsyncKeyState(VK_DOWN)&0x8000))lastInput=GetTickCount64();
            bool manual=GetTickCount64()-lastInput<10000;
            if(!manual && foreground==GetCurrentProcessId() && !game->isPaused() && frame%24==0 && lastCamera!=frame) {
                lastCamera=frame;game->setScreenPosition(clamp(center()-Position(320,200)));
            }
            game->setTextSize();
            game->drawTextScreen(6,26,"F1 Scout | F2 Raid | F3 Enemy Expansion | F4 Hold Here");
            game->drawTextScreen(6,40,"F5 Main Base | F6 Defend Base | F7 Regroup | F8 Disengage");
            game->drawTextScreen(6,56,"Main: %s | Scout: %s | Raid: %s",mainLabel(),scoutLabel(),raidLabel());
            game->drawTextScreen(6,70,"%s",feedbackLabel());
            game->drawTextScreen(6,84,"Army %d | Camera %s",int(army.size()),manual?"MANUAL":"AUTO");
        }
        if(frame%120==0) {
            Position c=center(),g=human?mainGoal():enemyStart;
            log<<"{\"event\":\"state\",\"frame\":"<<frame<<",\"stance\":\""<<(human?stance:"baseline")
               <<"\",\"workers\":"<<std::count_if(game->self()->getUnits().begin(),game->self()->getUnits().end(),[](Unit u){return u->getType().isWorker();})
               <<",\"army\":"<<army.size()<<",\"main_army\":"<<mainArmy().size()<<",\"center\":["<<c.x<<","<<c.y<<"],\"goal\":["<<g.x<<","<<g.y
               <<"],\"scout\":\""<<scoutStatus<<"\",\"raid\":\""<<harassStatus<<"\",\"raid_units\":"<<harassIds.size()
               <<",\"worker_guards\":"<<guards.size()<<",\"guard_peak\":"<<guardPeak<<",\"bait_overrides\":"<<baitOverrides<<"}"<<std::endl;
        }
    }
    void onEnd(bool winner) override {
        if(ended)return;ended=true;
        if(hook){UnhookWindowsHookEx(hook);hook=nullptr;}
        if(delegate)delegate->onEnd(winner);
        if(role=="human" && env("COMMANDER_RESULTS_WATCH")=="1") {
            HMODULE reporter=LoadLibraryA("bwapi-data/AI/ResultsWatcher.dll");
            auto start=reporter?reinterpret_cast<void(__cdecl*)()>(GetProcAddress(reporter,"StartResultsWatcher")):nullptr;
            if(start)start();
        }
        log<<"{\"event\":\"end\",\"frame\":"<<game->getFrameCount()<<",\"winner\":"<<(winner?"true":"false")<<"}"<<std::endl;
        log.close();std::ofstream("bwapi-data/write/match-ended.json")<<"{\"frame\":"<<game->getFrameCount()<<",\"winner\":"<<(winner?"true":"false")<<"}";
    }
    void onSendText(std::string text) override {if(delegate)delegate->onSendText(text);}
    void onReceiveText(Player p,std::string text) override {if(delegate)delegate->onReceiveText(p,text);}
    void onPlayerLeft(Player p) override {if(delegate)delegate->onPlayerLeft(p);}
    void onNukeDetect(Position p) override {if(delegate)delegate->onNukeDetect(p);}
    void onSaveGame(std::string name) override {if(delegate)delegate->onSaveGame(name);}
#define FORWARD(name) void name(Unit u) override {if(delegate)delegate->name(u);}
    FORWARD(onUnitDiscover) FORWARD(onUnitEvade) FORWARD(onUnitShow) FORWARD(onUnitHide)
    FORWARD(onUnitCreate) FORWARD(onUnitDestroy) FORWARD(onUnitMorph) FORWARD(onUnitRenegade) FORWARD(onUnitComplete)
#undef FORWARD
};
extern "C" void gameInit(Game* g){game=g;BroodwarPtr=g;}
extern "C" AIModule* newAIModule(){return new CommanderLayer();}
