#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <cmath>
#include <sstream>
#include <vector>

using namespace geode::prelude;

namespace fp {
    inline int stepCounter = 0;
    inline int lastInput[2] = {-1000000, -1000000};
    inline int count = 0;

    inline int currentFps() {
        double interval = CCDirector::get()->getAnimationInterval();
        if (interval <= 0.0) return 60;
        return static_cast<int>(std::round(1.0 / interval));
    }

    inline std::vector<int> parseFpsList() {
        std::vector<int> out;
        std::stringstream ss(Mod::get()->getSettingValue<std::string>("fps-list"));
        std::string item;
        while (std::getline(ss, item, ',')) {
            try {
                int v = std::stoi(item);
                if (v > 0) out.push_back(v);
            } catch (...) {}
        }
        return out;
    }

    inline bool fpsEnabled(int fps) {
        for (int v : parseFpsList()) {
            if (v == fps) return true;
        }
        return false;
    }

    inline void updateLabel() {
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_uiLayer) return;
        auto label = typeinfo_cast<CCLabelBMFont*>(pl->m_uiLayer->getChildByID("fp-label"_spr));
        if (!label) return;
        label->setString(fmt::format("FP: {} ({} FPS)", count, currentFps()).c_str());
    }

    inline void reset() {
        count = 0;
        lastInput[0] = lastInput[1] = -1000000;
        updateLabel();
    }
}

class $modify(FPBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        fp::stepCounter++;
        GJBaseGameLayer::processCommands(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (button != 1) return;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;

        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;

        int idx = isPlayer1 ? 0 : 1;
        int gap = fp::stepCounter - fp::lastInput[idx];
        fp::lastInput[idx] = fp::stepCounter;

        int fps = fp::currentFps();
        if (!fp::fpsEnabled(fps)) return;

        int tps = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tps"));
        int tolerance = static_cast<int>(Mod::get()->getSettingValue<int64_t>("tolerance"));
        int stepsPerFrame = std::max(1, static_cast<int>(std::round(static_cast<double>(tps) / fps)));

        if (gap >= 1 && gap <= stepsPerFrame + tolerance) {
            fp::count++;
            fp::updateLabel();

            if (Mod::get()->getSettingValue<bool>("play-sound")) {
                float vol = static_cast<float>(Mod::get()->getSettingValue<double>("volume"));
                FMODAudioEngine::get()->playEffect("perfect.wav"_spr, 1.f, 0.f, vol);
            }
        }
    }
};

class $modify(FPUILayer, UILayer) {
    bool init(GJBaseGameLayer* layer) {
        if (!UILayer::init(layer)) return false;

        auto label = CCLabelBMFont::create("FP: 0", "bigFont.fnt");
        label->setID("fp-label"_spr);
        label->setAnchorPoint({0.f, 1.f});
        label->setScale(0.5f);
        label->setOpacity(200);
        label->setZOrder(100);
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({8.f, win.height - 8.f});
        this->addChild(label);

        fp::count = 0;
        fp::lastInput[0] = fp::lastInput[1] = -1000000;
        return true;
    }
};

class $modify(FPPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        if (Mod::get()->getSettingValue<bool>("reset-on-restart")) {
            fp::reset();
        } else {
            fp::updateLabel();
        }
    }
};
