// Click Assist v1.4
// IDEA PRINCIPAL: corrección de tus clicks.
//   El mod estima cuál es el momento ideal para clickear.
//   - Click TEMPRANO: si clickeas hasta N frames antes (ajuste "early"), el mod RETRASA
//     el click hasta el momento ideal, para que pases el obstáculo.
//   - Click TARDÍO (opcional, ajuste "window"): te devuelve al momento ideal y aplica el click ahí.
// Extras: modo Aviso (muestra CLICK!), modo Auto (el mod clickea), pendientes, orbs.
// SAFE MODE obligatorio: con el mod activo en modo normal no se guarda ni completado ni %.
// Aproximado: no simula la física, no maneja pads, portales ni dual.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float kBaseSpeed    = 311.58f; // unidades/seg a velocidad 1x
    constexpr float kReaction     = 0.15f;   // en modo Aviso, el cue sale 150 ms antes
    constexpr float kHoldTime     = 0.07f;   // duración del click en modo Auto
    constexpr float kFireCooldown = 0.10f;   // mínimo entre clicks
    constexpr float kLaneTol      = 11.f;    // tolerancia vertical del "carril" del jugador
    constexpr float kMaxW         = 240.f;   // ancho máximo de objeto que se considera
    constexpr int   kOrbBase      = 1000000; // claves de objetivos
    constexpr int   kJumpBase     = 2000000;

    // Bloque, hazard o pendiente
    struct Ob {
        float x0, x1, y0, y1;
        bool hazard;
        bool slope;
        float hxA, hyA, hxB, hyB; // hipotenusa (solo pendientes)
        bool solidBelow;          // true: lo sólido está debajo de la hipotenusa
    };

    struct Orb {
        float x, y;
    };

    enum class Veh { Cube, Ship, Ufo, Wave, Ball, Robot, Spider, Swing };

    Veh vehicleOf(PlayerObject* p) {
        if (p->m_isShip)   return Veh::Ship;
        if (p->m_isBird)   return Veh::Ufo;
        if (p->m_isDart)   return Veh::Wave;
        if (p->m_isBall)   return Veh::Ball;
        if (p->m_isRobot)  return Veh::Robot;
        if (p->m_isSpider) return Veh::Spider;
        if (p->m_isSwing)  return Veh::Swing;
        return Veh::Cube;
    }

    bool isGroundVeh(Veh v) {
        return v == Veh::Cube || v == Veh::Robot || v == Veh::Ball || v == Veh::Spider;
    }

    float leadFor(Veh v) {
        switch (v) {
            case Veh::Ball:   return 38.f;
            case Veh::Spider: return 28.f;
            default:          return 55.f; // cubo y robot
        }
    }

    // Orbs por tipo (amarillo, rosa, azul) o por ID (verde, rojo, negro, dash, custom)
    bool isOrb(GameObject* obj) {
        int t = static_cast<int>(obj->m_objectType);
        if (t == 11 || t == 12 || t == 13) return true;
        switch (obj->m_objectID) {
            case 36: case 84: case 141: case 1022: case 1330:
            case 1333: case 1594: case 1704: case 1751:
                return true;
            default:
                return false;
        }
    }

    // Primer obstáculo en mi carril dentro de 'lead' unidades (cubo/robot/bola/araña)
    int findAhead(std::vector<Ob> const& v, float px, float py, float lead, int skip) {
        auto it = std::lower_bound(v.begin(), v.end(), px,
            [](Ob const& o, float val) { return o.x0 < val; });
        for (; it != v.end() && (it->x0 - px) <= lead; ++it) {
            if (it->slope && !it->hazard) continue;           // las pendientes normales no se saltan
            if (it->y1 < py - kLaneTol || it->y0 > py + kLaneTol) continue;
            int idx = static_cast<int>(it - v.begin());
            if (idx == skip) continue;
            return idx;
        }
        return -1;
    }

    // Orb que estoy tocando (o por tocar) a mi altura
    int findOrb(std::vector<Orb> const& v, float px, float pyPred, float lead, int skip) {
        auto it = std::lower_bound(v.begin(), v.end(), px - 12.f,
            [](Orb const& o, float val) { return o.x < val; });
        for (; it != v.end() && (it->x - px) <= lead; ++it) {
            if (std::abs(it->y - pyPred) > 34.f) continue;
            int idx = static_cast<int>(it - v.begin());
            if (idx == skip) continue;
            return idx;
        }
        return -1;
    }

    // Orb que está más arriba y se alcanza con un salto desde el suelo
    int findOrbJump(std::vector<Orb> const& v, float px, float py, float up, float lead, int skip) {
        auto it = std::lower_bound(v.begin(), v.end(), px,
            [](Orb const& o, float val) { return o.x < val; });
        for (; it != v.end() && (it->x - px) <= lead; ++it) {
            float h = up * (it->y - py);
            if (h < 20.f || h > 110.f) continue;
            int idx = static_cast<int>(it - v.begin());
            if (idx == skip) continue;
            return idx;
        }
        return -1;
    }

    // Piso y techo que hay en la columna x (bloques y pendientes)
    void columnBounds(std::vector<Ob> const& v, float x, float py, float& floorY, float& ceilY) {
        floorY = -1e9f;
        ceilY = 1e9f;
        auto it = std::lower_bound(v.begin(), v.end(), x - kMaxW,
            [](Ob const& o, float val) { return o.x0 < val; });
        for (; it != v.end() && it->x0 <= x; ++it) {
            if (it->x1 < x) continue;
            float lo = it->y0, hi = it->y1;
            if (it->slope) {
                float dx = it->hxB - it->hxA;
                float t = (std::abs(dx) > 0.001f) ? (x - it->hxA) / dx : 0.f;
                t = std::clamp(t, 0.f, 1.f);
                float hy = it->hyA + t * (it->hyB - it->hyA);
                if (it->solidBelow) hi = hy; else lo = hy;
            }
            float pad = it->hazard ? 8.f : 0.f;
            float mid = (lo + hi) * 0.5f;
            if (mid >= py) ceilY = std::min(ceilY, lo - pad);
            else           floorY = std::max(floorY, hi + pad);
        }
    }

    // Altura objetivo = centro del pasillo D unidades más adelante
    bool corridorTarget(std::vector<Ob> const& v, float px, float py, float D, float& out) {
        float fl2, ce2;
        columnBounds(v, px + D, py, fl2, ce2);
        bool foundCeil  = ce2 < 1e8f;
        bool foundFloor = fl2 > -1e8f;

        float t;
        if (foundCeil && foundFloor) {
            t = (ce2 + fl2) * 0.5f;
        } else if (foundCeil && (ce2 - py) < 150.f) {
            t = ce2 - 75.f;
        } else if (foundFloor && (py - fl2) < 150.f) {
            t = fl2 + 75.f;
        } else {
            return false;
        }

        // No salirse del pasillo que tengo justo delante
        float fl1, ce1;
        columnBounds(v, px + D * 0.4f, py, fl1, ce1);
        float lo = (fl1 > -1e8f && (py - fl1) < 250.f) ? fl1 + 12.f : -1e9f;
        float hi = (ce1 < 1e8f && (ce1 - py) < 250.f) ? ce1 - 12.f : 1e9f;
        if (lo <= hi) t = std::clamp(t, lo, hi);

        out = t;
        return true;
    }
}

class $modify(AssistPlayLayer, PlayLayer) {
public:
    struct Fields {
        std::vector<Ob> obs;           // bloques, hazards y pendientes, ordenados por X
        std::vector<Orb> orbs;         // orbs, ordenados por X
        int lastCueKey = -1;
        int lastIdealKey = -1;
        float releaseTimer = -1.f;
        float cooldown = 0.f;
        float lastPy = 0.f;
        bool hasLastPy = false;
        float holdY = 0.f;
        bool hasHoldY = false;
        bool held = false;             // botón realmente pulsado por el mod (Auto)
        bool desired = false;          // estado deseado en vehículos de mantener
        int lastCue = -1;
        bool assisted = false;         // el mod estuvo activo en modo normal este intento

        // Corrección de clicks tardíos
        float clock = 0.f;
        bool userDown = false;         // si TÚ tienes el botón apretado
        bool injecting = false;        // el mod está enviando un click propio
        bool pressArmed = false;
        bool releaseArmed = false;
        float pressT = 0.f;
        float releaseT = 0.f;

        // Retraso de clicks tempranos
        bool pending = false;          // hay un click tuyo esperando el momento ideal
        bool pendingRelease = false;   // soltaste el botón mientras esperaba
        float pendingT = 0.f;
        float noArmUntil = 0.f;
        float lastVy = 0.f;
        PlayerCheckpoint* snapPress = nullptr;
        PlayerCheckpoint* snapRelease = nullptr;

        CCLabelBMFont* cue = nullptr;
        CCLabelBMFont* safe = nullptr;

        ~Fields() {
            if (snapPress) snapPress->release();
            if (snapRelease) snapRelease->release();
        }
    };

    // ---------- utilidades de texto en pantalla ----------
    void flashCue(char const* txt) {
        if (auto c = m_fields->cue) {
            c->setString(txt);
            c->stopAllActions();
            c->setOpacity(255);
            c->runAction(CCFadeOut::create(0.35f));
            m_fields->lastCue = -1;
        }
    }

    void holdCue(bool want) {
        auto c = m_fields->cue;
        if (!c) return;
        int st = want ? 1 : 0;
        if (m_fields->lastCue == st) return;
        m_fields->lastCue = st;
        c->stopAllActions();
        c->setString(want ? "HOLD" : "RELEASE");
        c->setOpacity(255);
    }

    void hideCue() {
        if (m_fields->lastCue != -1) {
            m_fields->lastCue = -1;
            if (m_fields->cue) {
                m_fields->cue->stopAllActions();
                m_fields->cue->setOpacity(0);
            }
        }
    }

    // Click propio del mod (no cuenta como input tuyo)
    void sendButton(bool down) {
        m_fields->injecting = true;
        this->handleButton(down, static_cast<int>(PlayerButton::Jump), true);
        m_fields->injecting = false;
    }

    // ---------- CORRECCIÓN DE TUS CLICKS ----------
    // Segundos hasta el próximo momento ideal (si cae dentro de maxSec), o -1
    float futureIdealDelay(float maxSec) {
        auto p = m_player1;
        if (!p || p->m_isDead) return -1.f;
        Veh veh = vehicleOf(p);
        if (veh == Veh::Ship || veh == Veh::Wave) return -1.f;

        auto mod = Mod::get();
        float corr = static_cast<float>(mod->getSettingValue<double>("correction")) / 1000.f;
        bool canOrb = mod->getSettingValue<bool>("orbs");
        float px = p->getPositionX();
        float py = p->getPositionY();
        float vy = m_fields->lastVy;
        float rel = p->m_playerSpeed / 0.9f;
        float ups = kBaseSpeed * rel;
        float best = -1.f;

        // Obstáculos (cubo, robot, bola, araña en el suelo)
        if (isGroundVeh(veh) && p->m_isOnGround) {
            float lead = leadFor(veh) * rel + corr * ups;
            auto& v = m_fields->obs;
            // si ya hay uno dentro del alcance, tu click ya va a tiempo: no se retrasa
            if (findAhead(v, px, py, lead, -1) >= 0) return -1.f;
            auto it = std::lower_bound(v.begin(), v.end(), px,
                [](Ob const& o, float val) { return o.x0 < val; });
            for (; it != v.end() && (it->x0 - px) <= lead + ups * maxSec; ++it) {
                if (it->slope && !it->hazard) continue;
                if (it->y1 < py - kLaneTol || it->y0 > py + kLaneTol) continue;
                best = (it->x0 - px - lead) / ups;
                break;
            }
        }

        // Orbs
        if (canOrb) {
            float leadO = 24.f * rel + corr * ups;
            auto& ov = m_fields->orbs;
            auto it = std::lower_bound(ov.begin(), ov.end(), px,
                [](Orb const& o, float val) { return o.x < val; });
            for (; it != ov.end() && (it->x - px) <= leadO + ups * maxSec; ++it) {
                float t = (it->x - px - leadO) / ups;
                if (t <= 0.f) continue;
                float pyPred = py + vy * t;
                if (std::abs(it->y - pyPred) > 34.f) continue;
                if (best < 0.f || t < best) best = t;
                break;
            }
        }

        if (best > 0.f && best <= maxSec) return best;
        return -1.f;
    }

    // Se llama cada vez que TÚ aprietas o sueltas el botón (antes de que el juego lo procese).
    // Devuelve true si ese click NO debe llegar al juego (porque el mod lo retrasa).
    bool onUserInput(bool down) {
        if (m_fields->injecting) return false;
        m_fields->userDown = down;

        // Si ya hay un click retrasado esperando, el resto de tus clicks se absorben
        if (m_fields->pending) {
            if (!down) m_fields->pendingRelease = true;
            return true;
        }

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return false;
        if (mod->getSettingValue<std::string>("mode") == "Auto") return false;

        auto p = m_player1;
        if (!p || p->m_isDead) return false;

        // 1) Click TEMPRANO: se retrasa hasta el momento ideal
        int early = static_cast<int>(mod->getSettingValue<int64_t>("early"));
        if (down && early > 0) {
            float d = this->futureIdealDelay(static_cast<float>(early) / 60.f);
            if (d > 0.f) {
                m_fields->pending = true;
                m_fields->pendingRelease = false;
                m_fields->pendingT = m_fields->clock + d;
                int f = std::max(1, static_cast<int>(std::lround(d * 60.f)));
                std::string txt = "DELAYED +" + std::to_string(f) + "f";
                this->flashCue(txt.c_str());
                return true;
            }
        }

        // 2) Click TARDÍO (opcional): volver al momento ideal y aplicar el click ahí
        int frames = static_cast<int>(mod->getSettingValue<int64_t>("window"));
        if (frames <= 0) return false;
        float win = static_cast<float>(frames) / 60.f;

        bool& armed = down ? m_fields->pressArmed : m_fields->releaseArmed;
        float armT = down ? m_fields->pressT : m_fields->releaseT;
        auto snap = down ? m_fields->snapPress : m_fields->snapRelease;
        if (!armed) return false;
        armed = false;                       // un solo uso por momento ideal

        float late = m_fields->clock - armT; // cuánto te pasaste del momento ideal
        if (late <= 0.f || late > win || !snap) return false;
        if (isGroundVeh(vehicleOf(p)) && !p->m_isOnGround) return false;

        p->loadFromCheckpoint(snap);         // volver al estado del momento ideal
        int lateFrames = std::max(1, static_cast<int>(std::lround(late * 60.f)));
        std::string txt = "FIXED +" + std::to_string(lateFrames) + "f";
        this->flashCue(txt.c_str());
        return false;
    }

    // >>> FIN DE LA PARTE 1: pega la PARTE 2 justo debajo de esta línea <<<
    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (isOrb(obj)) {
                m_fields->orbs.push_back({ obj->getPositionX(), obj->getPositionY() });
                continue;
            }

            auto t = obj->m_objectType;
            bool isSlope = (t == GameObjectType::Slope);
            if (t != GameObjectType::Hazard && t != GameObjectType::Solid && !isSlope) continue;

            auto r = obj->getObjectRect();
            float w = r.getMaxX() - r.getMinX();
            float h = r.getMaxY() - r.getMinY();
            if (w < 1.f || h < 1.f) {
                auto pos = obj->getPosition();
                r = CCRect(pos.x - 15.f, pos.y - 15.f, 30.f, 30.f);
            }

            Ob ob{};
            ob.x0 = r.getMinX();
            ob.x1 = r.getMaxX();
            ob.y0 = r.getMinY();
            ob.y1 = r.getMaxY();
            ob.hazard = (t == GameObjectType::Hazard);

            if (isSlope) {
                ob.slope = true;
                ob.hazard = obj->m_slopeIsHazard;
                // Esquina del ángulo recto y la hipotenusa opuesta
                bool wallLeft = obj->slopeWallLeft();
                bool floorTop = obj->slopeFloorTop();
                float cx = wallLeft ? ob.x0 : ob.x1;
                float ox = wallLeft ? ob.x1 : ob.x0;
                float cy = floorTop ? ob.y1 : ob.y0;
                float oy = floorTop ? ob.y0 : ob.y1;
                ob.hxA = cx; ob.hyA = oy;
                ob.hxB = ox; ob.hyB = cy;
                ob.solidBelow = !floorTop;
            }
            m_fields->obs.push_back(ob);
        }
        std::sort(m_fields->obs.begin(), m_fields->obs.end(),
            [](Ob const& a, Ob const& b) { return a.x0 < b.x0; });
        std::sort(m_fields->orbs.begin(), m_fields->orbs.end(),
            [](Orb const& a, Orb const& b) { return a.x < b.x; });

        // Dos "fotos" del jugador para la corrección
        m_fields->snapPress = PlayerCheckpoint::create();
        m_fields->snapPress->retain();
        m_fields->snapRelease = PlayerCheckpoint::create();
        m_fields->snapRelease->retain();

        auto win = CCDirector::get()->getWinSize();

        auto cue = CCLabelBMFont::create("CLICK!", "bigFont.fnt");
        cue->setPosition({ win.width / 2.f, win.height * 0.75f });
        cue->setScale(0.9f);
        cue->setOpacity(0);
        this->addChild(cue, 1000);
        m_fields->cue = cue;

        auto safe = CCLabelBMFont::create("SAFE MODE", "bigFont.fnt");
        safe->setPosition({ win.width / 2.f, win.height - 14.f });
        safe->setScale(0.35f);
        safe->setOpacity(0);
        this->addChild(safe, 1000);
        m_fields->safe = safe;
    }

    void resetLevel() {
        if (m_fields->held || m_fields->releaseTimer > 0.f) {
            this->sendButton(false);
        }
        PlayLayer::resetLevel();
        m_fields->lastCueKey = -1;
        m_fields->lastIdealKey = -1;
        m_fields->releaseTimer = -1.f;
        m_fields->cooldown = 0.f;
        m_fields->hasLastPy = false;
        m_fields->hasHoldY = false;
        m_fields->held = false;
        m_fields->desired = false;
        m_fields->lastCue = -1;
        m_fields->assisted = false;
        m_fields->pressArmed = false;
        m_fields->releaseArmed = false;
        m_fields->pending = false;
        m_fields->pendingRelease = false;
        if (m_fields->cue) { m_fields->cue->stopAllActions(); m_fields->cue->setOpacity(0); }
        if (m_fields->safe) m_fields->safe->setOpacity(0);
    }

    // ---------------- SAFE MODE ----------------
    bool safeActive() {
        if (m_isPracticeMode || m_isTestMode) return false; // ya no cuentan de todos modos
        return m_fields->assisted || Mod::get()->getSettingValue<bool>("enabled");
    }

    void levelComplete() {
        bool oldTest = m_isTestMode;
        if (this->safeActive()) {
            m_isTestMode = true; // el juego trata el intento como "test": no guarda nada
            Notification::create("Safe Mode: completado NO contado", NotificationIcon::Warning, 3.f)->show();
        }
        PlayLayer::levelComplete();
        m_isTestMode = oldTest;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        bool oldTest = m_isTestMode;
        if (this->safeActive()) m_isTestMode = true; // tampoco se guarda el % mejor
        PlayLayer::destroyPlayer(player, object);
        m_isTestMode = oldTest;
    }

    // ---------------- ASISTENTE ----------------
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        m_fields->clock += dt;

        // Soltar el click (modo Auto)
        if (m_fields->releaseTimer > 0.f) {
            m_fields->releaseTimer -= dt;
            if (m_fields->releaseTimer <= 0.f) {
                m_fields->releaseTimer = -1.f;
                this->sendButton(false);
            }
        }
        if (m_fields->cooldown > 0.f) m_fields->cooldown -= dt;

        // Click retrasado: se envía justo en el momento ideal
        if (m_fields->pending && m_fields->clock >= m_fields->pendingT) {
            m_fields->pending = false;
            this->sendButton(true);
            if (m_fields->pendingRelease) m_fields->releaseTimer = kHoldTime;
            m_fields->noArmUntil = m_fields->clock + 0.25f;
            m_fields->pressArmed = false;
        }

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) {
            if (m_fields->held) { this->sendButton(false); m_fields->held = false; }
            this->hideCue();
            return;
        }

        // SAFE MODE: marcar el intento si estamos en modo normal
        if (!m_isPracticeMode && !m_fields->assisted) {
            m_fields->assisted = true;
            if (m_fields->safe) m_fields->safe->setOpacity(160);
        }

        if (m_levelSettings->m_platformerMode) return;

        auto p = m_player1;
        if (!p || p->m_isDead) return;

        std::string mode = mod->getSettingValue<std::string>("mode");
        bool autoMode = (mode == "Auto");
        bool showCue = (mode == "Aviso");
        int winFrames = static_cast<int>(mod->getSettingValue<int64_t>("window"));
        float corr = static_cast<float>(mod->getSettingValue<double>("correction")) / 1000.f;

        if (!autoMode && m_fields->held) { this->sendButton(false); m_fields->held = false; }

        float px = p->getPositionX();
        float py = p->getPositionY();
        float vy = 0.f;
        if (m_fields->hasLastPy) vy = (py - m_fields->lastPy) / std::max(dt, 0.0001f);
        m_fields->lastPy = py;
        m_fields->hasLastPy = true;
        m_fields->lastVy = vy;

        float rel = p->m_playerSpeed / 0.9f; // 0.9 = velocidad 1x
        float ups = kBaseSpeed * rel;
        float up = p->m_isUpsideDown ? -1.f : 1.f;
        Veh veh = vehicleOf(p);
        bool groundVeh = isGroundVeh(veh);
        bool onGround = p->m_isOnGround;
        auto& obs = m_fields->obs;

        bool canOrb = mod->getSettingValue<bool>("orbs") && veh != Veh::Ship && veh != Veh::Wave;

        // Guardar "foto" del jugador en el momento ideal (para corregir clicks tardíos)
        auto armPress = [&]() {
            if (autoMode || winFrames <= 0 || m_fields->userDown || !m_fields->snapPress) return;
            if (m_fields->clock < m_fields->noArmUntil) return;
            p->saveToCheckpoint(m_fields->snapPress);
            m_fields->pressArmed = true;
            m_fields->pressT = m_fields->clock;
        };
        auto armRelease = [&]() {
            if (autoMode || winFrames <= 0 || !m_fields->userDown || !m_fields->snapRelease) return;
            p->saveToCheckpoint(m_fields->snapRelease);
            m_fields->releaseArmed = true;
            m_fields->releaseT = m_fields->clock;
        };
        auto fire = [&]() {
            this->flashCue("AUTO");
            this->sendButton(true);
            m_fields->releaseTimer = kHoldTime;
            m_fields->cooldown = kFireCooldown;
        };

        // Siguiente objetivo (orb o obstáculo). extraSec = adelanto extra (para el aviso)
        auto pick = [&](float extraSec, int skipKey) -> int {
            float look = corr + extraSec;
            int skipObs  = (skipKey >= 0 && skipKey < kOrbBase) ? skipKey : -1;
            int skipOrb  = (skipKey >= kOrbBase && skipKey < kJumpBase) ? skipKey - kOrbBase : -1;
            int skipJump = (skipKey >= kJumpBase) ? skipKey - kJumpBase : -1;

            if (canOrb) {
                float pyPred = py + vy * look;
                float leadO = 24.f * rel + look * ups;
                int io = findOrb(m_fields->orbs, px, pyPred, leadO, skipOrb);
                if (io >= 0) return kOrbBase + io;
            }
            if (groundVeh && onGround) {
                float lead = leadFor(veh) * rel + look * ups;
                int idx = findAhead(obs, px, py, lead, skipObs);
                if (idx >= 0) return idx;
                if (canOrb) {
                    int oj = findOrbJump(m_fields->orbs, px, py, up, lead, skipJump);
                    if (oj >= 0) return kJumpBase + oj;
                }
            }
            return -1;
        };

        if (groundVeh) {
            if (m_fields->held) { this->sendButton(false); m_fields->held = false; }
            this->hideCue();
        }

        // ---- Cubo/robot/bola/araña (obstáculos y orbs) y UFO/swing (orbs) ----
        if (veh != Veh::Ship && veh != Veh::Wave) {
            if (autoMode) {
                if (m_fields->cooldown <= 0.f) {
                    int k = pick(0.f, m_fields->lastIdealKey);
                    if (k >= 0) {
                        m_fields->lastIdealKey = k;
                        fire();
                        return;
                    }
                }
            } else {
                if (showCue) {
                    int kc = pick(kReaction, m_fields->lastCueKey);
                    if (kc >= 0) {
                        m_fields->lastCueKey = kc;
                        this->flashCue("CLICK!");
                    }
                }
                int ki = pick(0.f, m_fields->lastIdealKey);
                if (ki >= 0) {
                    m_fields->lastIdealKey = ki;
                    armPress();
                }
            }
            if (groundVeh) return;
        }

        // ---- Nave, UFO, wave, swing: seguir el centro del pasillo (con pendientes) ----
        float D = ((veh == Veh::Wave) ? 70.f : 100.f) * rel;
        float target;
        if (corridorTarget(obs, px, py, D, target)) {
            m_fields->holdY = target;
            m_fields->hasHoldY = true;
        } else {
            if (!m_fields->hasHoldY) { m_fields->holdY = py; m_fields->hasHoldY = true; }
            target = m_fields->holdY;
        }

        float horizon = (veh == Veh::Wave) ? corr : (0.10f + corr);
        float pred = py + vy * horizon;
        float errUp = up * (target - pred); // >0 = hay que subir (respecto a la gravedad)
        float vyUp = up * vy;

        if (veh == Veh::Ship || veh == Veh::Wave) {
            float dz = (veh == Veh::Wave) ? 3.f : 8.f;
            bool want = m_fields->desired;
            if (errUp > dz) want = true;
            else if (errUp < -dz) want = false;

            if (want != m_fields->desired) {
                m_fields->desired = want;
                if (!autoMode) {
                    if (want) armPress(); else armRelease();
                }
            }

            if (autoMode) {
                if (want != m_fields->held) { this->sendButton(want); m_fields->held = want; }
            } else if (showCue) {
                this->holdCue(want);
            }
            return;
        }

        // UFO y swing: toques con enfriamiento
        this->hideCue();
        if (m_fields->cooldown > 0.f) return;

        bool tap = false;
        float cd = 0.15f;
        if (veh == Veh::Ufo) {
            tap = errUp > 8.f && vyUp < 150.f;
            cd = 0.18f;
        } else { // Swing: cada click invierte la gravedad
            tap = (vyUp < -20.f && errUp > 0.f) || (vyUp > 20.f && errUp < 0.f);
            cd = 0.12f;
        }
        if (!tap) return;

        if (autoMode) {
            fire();
        } else {
            if (showCue) this->flashCue("CLICK!");
            armPress();
        }
        m_fields->cooldown = cd;
    }
};

// Intercepta TUS clicks antes de que el juego los procese
class $modify(AssistBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (button == static_cast<int>(PlayerButton::Jump) && isPlayer1) {
            if (auto pl = PlayLayer::get()) {
                if (static_cast<GJBaseGameLayer*>(pl) == this) {
                    if (static_cast<AssistPlayLayer*>(pl)->onUserInput(down)) return;
                }
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

// FIN DE main.cpp
