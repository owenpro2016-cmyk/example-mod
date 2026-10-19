// Click Assist v1.1
// - Te avisa (o hace) el click/mantener en el momento ideal, en TODOS los vehículos.
// - Funciona en práctica y en modo normal.
// - SAFE MODE obligatorio: si el mod está activo en modo normal, ni el completado ni
//   el progreso (% mejor) de ese intento se guardan. No hay opción para desactivarlo.
// Aproximado: usa hazards y bloques sólidos. No cubre orbs, pads, portales, slopes ni dual.

#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr float kBaseSpeed = 311.58f; // unidades/seg a velocidad 1x
    constexpr float kReaction  = 0.15f;   // en modo Aviso, el cue sale 150 ms antes
    constexpr float kHoldTime  = 0.07f;   // duración del click en modo Auto
    constexpr float kLaneTol   = 11.f;    // tolerancia vertical del "carril" del jugador

    struct Ob {
        float x0, x1, y0, y1;
        bool hazard;
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

    float leadFor(Veh v) {
        switch (v) {
            case Veh::Ball:   return 38.f;
            case Veh::Spider: return 28.f;
            default:          return 55.f; // cubo y robot
        }
    }

    // Primer obstáculo en mi carril dentro de 'lead' unidades (para cubo/robot/bola/araña)
    int findAhead(std::vector<Ob> const& v, float px, float py, float lead, int skip) {
        auto it = std::lower_bound(v.begin(), v.end(), px,
            [](Ob const& o, float val) { return o.x0 < val; });
        for (; it != v.end() && (it->x0 - px) <= lead; ++it) {
            if (it->y1 < py - kLaneTol || it->y0 > py + kLaneTol) continue;
            int idx = static_cast<int>(it - v.begin());
            if (idx == skip) continue;
            return idx;
        }
        return -1;
    }

    // Altura objetivo = centro del pasillo que viene (nave, ufo, wave, swing)
    bool corridorTarget(std::vector<Ob> const& v, float px, float py, float L, float& out) {
        float ceilY = 1e9f, floorY = -1e9f;
        auto it = std::lower_bound(v.begin(), v.end(), px - 150.f,
            [](Ob const& o, float val) { return o.x0 < val; });
        for (; it != v.end() && it->x0 <= px + L; ++it) {
            if (it->x1 < px - 10.f) continue;
            float mid = (it->y0 + it->y1) * 0.5f;
            float pad = it->hazard ? 8.f : 0.f;
            if (mid >= py) ceilY = std::min(ceilY, it->y0 - pad);
            else           floorY = std::max(floorY, it->y1 + pad);
        }
        bool hasCeil  = (ceilY - py) < 150.f;
        bool hasFloor = (py - floorY) < 150.f;
        if (hasCeil && hasFloor) out = (ceilY + floorY) * 0.5f;
        else if (hasCeil)        out = ceilY - 75.f;
        else if (hasFloor)       out = floorY + 75.f;
        else return false;
        return true;
    }
}

class $modify(AssistPlayLayer, PlayLayer) {
    struct Fields {
        std::vector<Ob> obs;           // hazards + sólidos, ordenados por X
        int lastTarget = -1;
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
        CCLabelBMFont* cue = nullptr;
        CCLabelBMFont* safe = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            auto t = obj->m_objectType;
            if (t != GameObjectType::Hazard && t != GameObjectType::Solid) continue;
            auto r = obj->getObjectRect();
            float w = r.getMaxX() - r.getMinX();
            float h = r.getMaxY() - r.getMinY();
            if (w < 1.f || h < 1.f) {
                auto pos = obj->getPosition();
                r = CCRect(pos.x - 15.f, pos.y - 15.f, 30.f, 30.f);
            }
            m_fields->obs.push_back({ r.getMinX(), r.getMaxX(), r.getMinY(), r.getMaxY(),
                                      t == GameObjectType::Hazard });
        }
        std::sort(m_fields->obs.begin(), m_fields->obs.end(),
            [](Ob const& a, Ob const& b) { return a.x0 < b.x0; });

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
            this->handleButton(false, static_cast<int>(PlayerButton::Jump), true);
        }
        PlayLayer::resetLevel();
        m_fields->lastTarget = -1;
        m_fields->releaseTimer = -1.f;
        m_fields->cooldown = 0.f;
        m_fields->hasLastPy = false;
        m_fields->hasHoldY = false;
        m_fields->held = false;
        m_fields->desired = false;
        m_fields->lastCue = -1;
        m_fields->assisted = false;
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

        auto btn = [this](bool down) {
            this->handleButton(down, static_cast<int>(PlayerButton::Jump), true);
        };
        auto flash = [this](char const* txt) {
            if (auto c = m_fields->cue) {
                c->setString(txt);
                c->stopAllActions();
                c->setOpacity(255);
                c->runAction(CCFadeOut::create(0.35f));
                m_fields->lastCue = -1;
            }
        };
        auto holdCue = [this](bool want) {
            auto c = m_fields->cue;
            if (!c) return;
            int st = want ? 1 : 0;
            if (m_fields->lastCue == st) return;
            m_fields->lastCue = st;
            c->stopAllActions();
            c->setString(want ? "HOLD" : "RELEASE");
            c->setOpacity(255);
        };
        auto hideCue = [this]() {
            if (m_fields->lastCue != -1) {
                m_fields->lastCue = -1;
                if (m_fields->cue) { m_fields->cue->stopAllActions(); m_fields->cue->setOpacity(0); }
            }
        };

        // Soltar el click (modo Auto)
        if (m_fields->releaseTimer > 0.f) {
            m_fields->releaseTimer -= dt;
            if (m_fields->releaseTimer <= 0.f) {
                m_fields->releaseTimer = -1.f;
                btn(false);
            }
        }
        if (m_fields->cooldown > 0.f) m_fields->cooldown -= dt;

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) {
            if (m_fields->held) { btn(false); m_fields->held = false; }
            hideCue();
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

        bool autoMode = mod->getSettingValue<std::string>("mode") == "Auto";
        float corr = static_cast<float>(mod->getSettingValue<double>("correction")) / 1000.f;

        if (!autoMode && m_fields->held) { btn(false); m_fields->held = false; }

        float px = p->getPositionX();
        float py = p->getPositionY();
        float vy = 0.f;
        if (m_fields->hasLastPy) vy = (py - m_fields->lastPy) / std::max(dt, 0.0001f);
        m_fields->lastPy = py;
        m_fields->hasLastPy = true;

        float rel = p->m_playerSpeed / 0.9f; // 0.9 = velocidad 1x
        float ups = kBaseSpeed * rel;
        Veh veh = vehicleOf(p);
        auto& obs = m_fields->obs;

        // ---- Cubo, robot, bola, araña: click antes del obstáculo ----
        if (veh == Veh::Cube || veh == Veh::Robot || veh == Veh::Ball || veh == Veh::Spider) {
            if (m_fields->held) { btn(false); m_fields->held = false; }
            hideCue();
            if (!p->m_isOnGround) return;

            float lead = leadFor(veh) * rel + corr * ups;
            if (!autoMode) lead += kReaction * ups;

            int idx = findAhead(obs, px, py, lead, m_fields->lastTarget);
            if (idx < 0) return;
            m_fields->lastTarget = idx;

            flash(autoMode ? "AUTO" : "CLICK!");
            if (autoMode) {
                btn(true);
                m_fields->releaseTimer = kHoldTime;
            }
            return;
        }

        // ---- Nave, UFO, wave, swing: seguir el centro del pasillo ----
        float L = (veh == Veh::Wave ? 130.f : 220.f) * rel;
        float target;
        if (corridorTarget(obs, px, py, L, target)) {
            m_fields->holdY = target;
            m_fields->hasHoldY = true;
        } else {
            if (!m_fields->hasHoldY) { m_fields->holdY = py; m_fields->hasHoldY = true; }
            target = m_fields->holdY;
        }

        float up = p->m_isUpsideDown ? -1.f : 1.f;
        float horizon = (veh == Veh::Wave) ? corr : (0.10f + corr);
        float pred = py + vy * horizon;
        float errUp = up * (target - pred); // >0 = hay que subir (respecto a la gravedad)
        float vyUp = up * vy;

        if (veh == Veh::Ship || veh == Veh::Wave) {
            float dz = (veh == Veh::Wave) ? 3.f : 8.f;
            bool want = m_fields->desired;
            if (errUp > dz) want = true;
            else if (errUp < -dz) want = false;
            m_fields->desired = want;

            if (autoMode) {
                if (want != m_fields->held) { btn(want); m_fields->held = want; }
            } else {
                holdCue(want);
            }
            return;
        }

        // UFO y swing: toques con enfriamiento
        hideCue();
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

        m_fields->cooldown = cd;
        flash(autoMode ? "AUTO" : "CLICK!");
        if (autoMode) {
            btn(true);
            m_fields->releaseTimer = kHoldTime;
        }
    }
};
