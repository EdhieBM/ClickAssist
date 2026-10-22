#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Windows.h>
#include <cstdio>
#include <algorithm>
#include <cstdarg>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

using namespace geode::prelude;

// Keep the live console for immediate debugging, but mirror every line to a
// deterministic file that can be reviewed after a gameplay session. This uses
// a normal C++ stream instead of redirecting printf globally, so it cannot
// interfere with Geode's loader or other mods during startup.
static std::ofstream s_telemetryFile;
static bool s_telemetryFileOpenAttempted = false;
static unsigned int s_telemetryPendingLines = 0;
constexpr unsigned int TELEMETRY_FLUSH_INTERVAL_LINES = 64;
constexpr char const* ASSISTED_STATUS_NODE_ID = "clickassist-assisted-status";
constexpr char const* ASSISTED_STAGE_NODE_ID = "clickassist-assisted-stage";

static void refreshAssistHud();

static bool isDetailedTelemetryEnabled() {
    return Mod::get()->getSettingValue<bool>("write-detailed-telemetry");
}

// The console deliberately stays small and useful during play. Detailed
// decisions are written through predictiveTelemetryPrintf below instead.
static void predictiveLogPrintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static void predictiveTelemetryPrintf(const char* format, ...) {
    if (!s_telemetryFile.is_open()) return;

    va_list args;
    va_start(args, format);
    char line[4096];
    const int written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written <= 0) return;

    const auto length = static_cast<std::streamsize>(
        std::min(written, static_cast<int>(sizeof(line) - 1))
    );
    s_telemetryFile.write(line, length);
    if (++s_telemetryPendingLines >= TELEMETRY_FLUSH_INTERVAL_LINES) {
        s_telemetryFile.flush();
        s_telemetryPendingLines = 0;
    }
}

static void flushTelemetryFile() {
    if (!s_telemetryFile.is_open()) return;
    s_telemetryFile.flush();
    s_telemetryPendingLines = 0;
}

static void openTelemetryFile() {
    if (s_telemetryFileOpenAttempted) return;
    s_telemetryFileOpenAttempted = true;

    if (!isDetailedTelemetryEnabled()) return;

    const auto logPath = Mod::get()->getSaveDir() / "prediction-telemetry.log";
    std::error_code error;
    std::filesystem::create_directories(logPath.parent_path(), error);

    const auto logPathString = logPath.string();
    s_telemetryFile.open(logPath, std::ios::out | std::ios::trunc);
    if (!s_telemetryFile.is_open()) {
        ::printf("[ClickAssist] Could not open detailed telemetry file: %s\\n",
            logPathString.c_str());
        return;
    }

    predictiveTelemetryPrintf("[ClickAssist] Detailed telemetry: %s\\n", logPathString.c_str());
}

// ============================================================================
//  USER SETTINGS
// ============================================================================

// True Wave Spam Guard: only bypasses the buffer if you do at least
// SPAM_STREAK_REQUIRED consecutive inputs faster than SPAM_INTERVAL_TICKS apart.
// This keeps isolated micro-clicks and spike/sawblade dodges 100% protected!
constexpr int SPAM_INTERVAL_TICKS = 8;
constexpr int SPAM_STREAK_REQUIRED = 3;

// A release assist is an emergency fallback, not a normal correction. It
// needs a sustained physical hold, a near-term collision, a major simulated
// improvement, and confirmation across two rendered frames.
constexpr int WAVE_RELEASE_ASSIST_MIN_HOLD_TICKS = 32;
constexpr int ROBOT_RELEASE_ASSIST_MIN_HOLD_TICKS = 48;
constexpr int WAVE_RELEASE_ASSIST_MAX_DANGER_TICKS = 16;
constexpr int ROBOT_RELEASE_ASSIST_MAX_DANGER_TICKS = 20;
constexpr int WAVE_RELEASE_ASSIST_MIN_GAIN_TICKS = 24;
constexpr int ROBOT_RELEASE_ASSIST_MIN_GAIN_TICKS = 32;
constexpr int RELEASE_ASSIST_CONFIRMATION_TICKS = 8;

// A buffer must have runway beyond the ordinary prediction horizon and remain
// viable if its application time shifts one simulation tick later. This makes
// a delayed input a deliberate safety decision rather than a borderline save.
constexpr int BUFFER_CONFIDENCE_RUNWAY_TICKS = 6;
constexpr int BUFFER_CONFIDENCE_TIMING_TOLERANCE_TICKS = 1;

// Continuous prediction is deliberately budgeted. The cached threat is
// refreshed at 60 Hz while clear and every physics update as a collision gets
// close; it is never a background thread or a millisecond busy-loop.
constexpr int CONTINUOUS_PROBE_NORMAL_TICKS = 72;
constexpr int CONTINUOUS_PROBE_WAVE_TICKS = 56;
constexpr int CONTINUOUS_PROBE_MINI_WAVE_TICKS = 42;
constexpr int CONTINUOUS_PROBE_CLEAR_REFRESH_TICKS = 4;
constexpr int CONTINUOUS_PROBE_DANGER_REFRESH_TICKS = 1;
constexpr int CONTINUOUS_PROBE_DANGER_TICKS = 24;

// ============================================================================

class TrajectoryBufferSim {
private:
    enum class ReleaseTelemetryState {
        None,
        HoldTooShort,
        UnsupportedInteraction,
        HoldIsSafe,
        DangerTooFarAway,
        ReleaseDoesNotClear,
        ReleaseGainTooSmall,
        Confirming,
    };

    PlayerObject* m_simPlayer1 = nullptr;
    PlayerObject* m_simPlayer2 = nullptr;

    bool m_simulating = false;
    bool m_firingBuffered = false;
    bool m_simulationDead = false;
    bool m_simulationUnsupportedInteraction = false;
    bool m_releaseAssistActive = false;
    int m_releaseAssistConfirmationTicks = 0;
    ReleaseTelemetryState m_lastReleaseTelemetryState = ReleaseTelemetryState::None;

    // Tracks actual physical mouse state vs what PlayLayer currently has active
    bool m_physicalHolding = false;
    bool m_gameHolding = false;
    bool m_physicalIsPlayer1 = true;

    // F6 Cycle State (Starts on Stage 0: 2 ticks Normal / 2 ticks Wave)
    int  m_presetIndex = 0;
    bool m_enabled = true;
    int  m_maxTicksNormal = 2;
    int  m_maxTicksWave = 2;

    // Buffer state
    int  m_waitTicks = -1;          // -1 = inactive, >= 0 = countdown ticks, -2 = queued opposite tick
    bool m_bufferedPush = true;     // true = buffered PRESS, false = buffered RELEASE
    bool m_queuedOpposite = false;  // true if user quickly released/re-pressed during wait
    float m_frameDt = 0.25f;        // GD 240 TPS normalized dt is 0.25f

    // Streak-based Spam Guard state
    int  m_ticksSinceLastInput = 999;
    int  m_rapidInputStreak = 0;

    // Per-level telemetry. These are recorded in the session summary when the
    // player exits, which makes later tuning evidence-based instead of guesswork.
    bool m_telemetrySessionActive = false;
    int m_telemetryPredictedInputs = 0;
    int m_telemetryImmediateSafe = 0;
    int m_telemetryBufferedInputs = 0;
    int m_telemetryRawNoBetterDelay = 0;
    int m_telemetryUnsupportedFallbacks = 0;
    int m_telemetryConfidenceRejections = 0;
    int m_telemetryCandidatesTested = 0;
    int m_telemetrySimulatedPads = 0;
    int m_telemetryReleaseAssists = 0;
    int m_telemetryAdaptiveExtraTotal = 0;
    int m_telemetryAdaptiveExtraMaximum = 0;
    int m_telemetryContinuousProbes = 0;
    int m_telemetryContinuousThreats = 0;
    int m_telemetryContinuousEscalations = 0;
    int m_telemetryFailOpenInteractions = 0;

    // Simulation revisits the same nearby object many times. Retain its
    // address for one level so telemetry describes encounters, not each
    // individual simulation tick.
    std::vector<GameObject*> m_loggedSimulatedPads;
    std::vector<GameObject*> m_loggedFailOpenInteractions;

    // Cache of the currently held trajectory. `m_threatTicks` is decremented
    // every real physics update between refreshes, so it stays useful without
    // repeatedly simulating the entire future path.
    int m_threatTicks = -1;
    int m_threatHorizon = 0;
    int m_threatRefreshCountdown = 0;
    bool m_threatUnsupported = false;

    static const char* releaseTelemetryStateName(ReleaseTelemetryState state) {
        switch (state) {
            case ReleaseTelemetryState::HoldTooShort: return "hold-too-short";
            case ReleaseTelemetryState::UnsupportedInteraction: return "unsupported-interaction";
            case ReleaseTelemetryState::HoldIsSafe: return "hold-is-safe";
            case ReleaseTelemetryState::DangerTooFarAway: return "danger-too-far";
            case ReleaseTelemetryState::ReleaseDoesNotClear: return "release-does-not-clear";
            case ReleaseTelemetryState::ReleaseGainTooSmall: return "release-gain-too-small";
            case ReleaseTelemetryState::Confirming: return "confirming";
            default: return "none";
        }
    }

    void resetReleaseTelemetry() {
        m_lastReleaseTelemetryState = ReleaseTelemetryState::None;
    }

    // Emit a line when the release prediction changes state. Confirmation is
    // intentionally logged every tick because the short 1..8 sequence is the
    // evidence needed to judge whether an emergency release was warranted.
    void reportReleaseTelemetry(ReleaseTelemetryState state, bool isWave,
        int heldTicks, int releasedTicks, int horizon, int minimumGain) {
        if (state != ReleaseTelemetryState::Confirming &&
            state == m_lastReleaseTelemetryState) {
            return;
        }

        predictiveTelemetryPrintf("[Telemetry][Release] mode=%s state=%s hold=%d release=%d horizon=%d gain=%d required=%d confirm=%d/%d\\n",
            isWave ? "wave" : "robot", releaseTelemetryStateName(state),
            heldTicks, releasedTicks, horizon, releasedTicks - heldTicks,
            minimumGain, m_releaseAssistConfirmationTicks,
            RELEASE_ASSIST_CONFIRMATION_TICKS);
        m_lastReleaseTelemetryState = state;
    }

    void resetTelemetryCounters() {
        m_telemetryPredictedInputs = 0;
        m_telemetryImmediateSafe = 0;
        m_telemetryBufferedInputs = 0;
        m_telemetryRawNoBetterDelay = 0;
        m_telemetryUnsupportedFallbacks = 0;
        m_telemetryConfidenceRejections = 0;
        m_telemetryCandidatesTested = 0;
        m_telemetrySimulatedPads = 0;
        m_telemetryReleaseAssists = 0;
        m_telemetryAdaptiveExtraTotal = 0;
        m_telemetryAdaptiveExtraMaximum = 0;
        m_telemetryContinuousProbes = 0;
        m_telemetryContinuousThreats = 0;
        m_telemetryContinuousEscalations = 0;
        m_telemetryFailOpenInteractions = 0;
        m_loggedSimulatedPads.clear();
        m_loggedFailOpenInteractions.clear();
    }

    void resetContinuousThreat() {
        m_threatTicks = -1;
        m_threatHorizon = 0;
        m_threatRefreshCountdown = 0;
        m_threatUnsupported = false;
    }

    int getAdaptiveHorizon(PlayerObject* p1, PlayerObject* p2, bool isDual,
        int& baseHorizon, int& extraHorizon, float& playerSpeed) const {
        const bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);
        const bool isMiniWave = (p1->m_isDart && p1->m_vehicleSize < 1.0f) ||
                                (isDual && p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);
        baseHorizon = isMiniWave ? 28 : (isWave ? 38 : 48);
        playerSpeed = p1->m_playerSpeed;
        if (isDual && p2) {
            playerSpeed = std::max(playerSpeed, p2->m_playerSpeed);
        }

        // Wave must retain its original short, responsive decision window.
        // Extra look-ahead makes a turn feel delayed even when the eventual
        // path is safe, so adaptive horizons are intentionally non-Wave only.
        if (isWave) {
            extraHorizon = 0;
            return baseHorizon;
        }

        extraHorizon = playerSpeed >= 1.8f ? 8 : (playerSpeed >= 1.3f ? 4 : 0);
        extraHorizon = std::min(extraHorizon, isMiniWave ? 8 : 10);
        return baseHorizon + extraHorizon;
    }

    void printTelemetrySummary() const {
        if (!m_telemetrySessionActive) return;
        predictiveTelemetryPrintf("[Telemetry][Summary] predicted-inputs=%d immediate-safe=%d buffered=%d raw-no-better=%d unsupported=%d confidence-rejected=%d candidates=%d simulated-pads=%d fail-open-interactions=%d release-assists=%d adaptive-extra-total=%d adaptive-extra-max=%d continuous-probes=%d continuous-threats=%d continuous-escalations=%d\\n",
            m_telemetryPredictedInputs, m_telemetryImmediateSafe,
            m_telemetryBufferedInputs, m_telemetryRawNoBetterDelay,
            m_telemetryUnsupportedFallbacks, m_telemetryConfidenceRejections,
            m_telemetryCandidatesTested, m_telemetrySimulatedPads,
            m_telemetryFailOpenInteractions, m_telemetryReleaseAssists, m_telemetryAdaptiveExtraTotal,
            m_telemetryAdaptiveExtraMaximum, m_telemetryContinuousProbes,
            m_telemetryContinuousThreats, m_telemetryContinuousEscalations);
        flushTelemetryFile();
    }

    PlayerObject* createTrajectoryPlayer() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return nullptr;

        PlayerObject* player = PlayerObject::create(1, 1, pl, pl, true);
        player->setPosition({0, 105});
        player->setVisible(false);
        pl->m_objectLayer->addChild(player);
        return player;
    }

    void resetCollisionLog(PlayerObject* self) {
        self->m_collisionLogTop->removeAllObjects();
        self->m_collisionLogBottom->removeAllObjects();
        self->m_collisionLogLeft->removeAllObjects();
        self->m_collisionLogRight->removeAllObjects();
    }

    // Matches Show Trajectory's exact attribute + hitbox size synchronization
    void syncSimPlayer(PlayerObject* sim, PlayerObject* real) {
        sim->setVisible(false);
        sim->copyAttributes(real);
        sim->setScale(real->getScale());
        sim->setContentSize(real->getContentSize());
        sim->m_width = real->m_width;
        sim->m_height = real->m_height;
        sim->m_gravityMod = real->m_gravityMod;
        sim->m_isOnGround = real->m_isOnGround;
        sim->m_isUpsideDown = real->m_isUpsideDown;
        sim->m_vehicleSize = real->m_vehicleSize;
        sim->m_playerSpeed = real->m_playerSpeed;
    }

    float getValidDt() const {
        if (m_frameDt > 0.001f && m_frameDt < 2.0f) {
            return m_frameDt;
        }
        return 0.25f;
    }

    // These pads are resolved entirely by the player's collision physics. We
    // can therefore feed them to the clone through GD's normal collision path
    // without activating the real level, its triggers, or its visual effects.
    static bool canSimulateWorldInteraction(GameObject* object) {
        if (!object || object->m_isDisabled || object->m_isDisabled2 ||
            object->m_isGroupDisabled || object->m_isGroupDisabledTemp) {
            return false;
        }

        switch (object->m_objectType) {
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::RedJumpPad:
                return true;
            default:
                return false;
        }
    }

    // Any world-changing object not on the isolated whitelist is still an
    // explicit fail-open boundary. This prevents a prediction from changing
    // trigger, orb, portal, or teleport state in the live level.
    static bool requiresFailOpenSimulation(GameObject* object) {
        if (!object || object->m_isDisabled || object->m_isDisabled2 ||
            object->m_isGroupDisabled || object->m_isGroupDisabledTemp) {
            return false;
        }

        if (canSimulateWorldInteraction(object)) {
            return false;
        }

        switch (object->m_objectType) {
            case GameObjectType::InverseGravityPortal:
            case GameObjectType::NormalGravityPortal:
            case GameObjectType::ShipPortal:
            case GameObjectType::CubePortal:
            case GameObjectType::YellowJumpPad:
            case GameObjectType::PinkJumpPad:
            case GameObjectType::GravityPad:
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::InverseMirrorPortal:
            case GameObjectType::NormalMirrorPortal:
            case GameObjectType::BallPortal:
            case GameObjectType::RegularSizePortal:
            case GameObjectType::MiniSizePortal:
            case GameObjectType::UfoPortal:
            case GameObjectType::Modifier:
            case GameObjectType::Breakable:
            case GameObjectType::DualPortal:
            case GameObjectType::SoloPortal:
            case GameObjectType::WavePortal:
            case GameObjectType::RobotPortal:
            case GameObjectType::TeleportPortal:
            case GameObjectType::GreenRing:
            case GameObjectType::DropRing:
            case GameObjectType::SpiderPortal:
            case GameObjectType::RedJumpPad:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::SwingPortal:
            case GameObjectType::GravityTogglePortal:
            case GameObjectType::SpiderOrb:
            case GameObjectType::SpiderPad:
            case GameObjectType::TeleportOrb:
                return true;
            default:
                return object->m_isTrigger;
        }
    }

public:
    void init() {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return;

        m_simPlayer1 = createTrajectoryPlayer();
        m_simPlayer2 = createTrajectoryPlayer();
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_physicalHolding = false;
        m_gameHolding = false;
        m_physicalIsPlayer1 = true;
        m_simulating = false;
        m_firingBuffered = false;
        m_simulationUnsupportedInteraction = false;
        m_releaseAssistActive = false;
        m_releaseAssistConfirmationTicks = 0;
        resetReleaseTelemetry();
        resetContinuousThreat();
        m_ticksSinceLastInput = 999;
        m_rapidInputStreak = 0;
        resetTelemetryCounters();
        m_telemetrySessionActive = true;
    }

    void quit() {
        printTelemetrySummary();
        m_telemetrySessionActive = false;
        if (m_simPlayer1) {
            m_simPlayer1->removeFromParentAndCleanup(true);
            m_simPlayer1 = nullptr;
        }
        if (m_simPlayer2) {
            m_simPlayer2->removeFromParentAndCleanup(true);
            m_simPlayer2 = nullptr;
        }
        m_simulating = false;
        m_firingBuffered = false;
        m_simulationUnsupportedInteraction = false;
        m_releaseAssistActive = false;
        m_releaseAssistConfirmationTicks = 0;
        resetReleaseTelemetry();
        resetContinuousThreat();
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_rapidInputStreak = 0;
    }

    // Cycles through the five configured keybind stages:
    // 0: 2 All / 2 Wave -> 1: 4 All / 4 Wave -> 2: 8 All / 8 Wave -> 3: 16 All / 8 Wave -> 4: OFF
    void cyclePreset() {
        m_presetIndex = (m_presetIndex + 1) % 5;
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_releaseAssistActive = false;
        m_releaseAssistConfirmationTicks = 0;
        resetReleaseTelemetry();

        switch (m_presetIndex) {
            case 0:
                m_enabled = true;
                m_maxTicksNormal = 2;
                m_maxTicksWave = 2;
                predictiveLogPrintf("[ClickAssist] F6 Cycle -> STAGE 1: 2 Ticks All | 2 Ticks Wave (0.5f @ 60fps)\n");
                break;
            case 1:
                m_enabled = true;
                m_maxTicksNormal = 4;
                m_maxTicksWave = 4;
                predictiveLogPrintf("[ClickAssist] F6 Cycle -> STAGE 2: 4 Ticks All | 4 Ticks Wave (1.0f @ 60fps)\n");
                break;
            case 2:
                m_enabled = true;
                m_maxTicksNormal = 8;
                m_maxTicksWave = 8;
                predictiveLogPrintf("[ClickAssist] F6 Cycle -> STAGE 3: 8 Ticks All | 8 Ticks Wave (2.0f @ 60fps)\n");
                break;
            case 3:
                m_enabled = true;
                m_maxTicksNormal = 16;
                m_maxTicksWave = 8;
                predictiveLogPrintf("[ClickAssist] F6 Cycle -> STAGE 4: 16 Ticks All | 8 Ticks Wave (4.0f / 2.0f @ 60fps)\n");
                break;
            case 4:
                m_enabled = false;
                m_maxTicksNormal = 0;
                m_maxTicksWave = 0;
                predictiveLogPrintf("[ClickAssist] F6 Cycle -> STAGE 5: OFF (100%% Raw Input)\n");
                break;
        }

        refreshAssistHud();
    }

    bool isSimulating() const { return m_simulating; }
    bool isFiringBuffered() const { return m_firingBuffered; }
    bool isEnabled() const { return m_enabled; }

    char const* getActiveTickLabel() const {
        switch (m_presetIndex) {
            case 0: return "2/2";
            case 1: return "4/4";
            case 2: return "8/8";
            case 3: return "16/8";
            default: return "";
        }
    }

    bool checkSimulationDeath(PlayerObject* player) {
        if (m_simulating && (player == m_simPlayer1 || player == m_simPlayer2)) {
            m_simulationDead = true;
            return true;
        }
        // Real player died -> clear any pending buffer so respawn is 100% clean
        m_waitTicks = -1;
        m_queuedOpposite = false;
        m_gameHolding = false;
        m_releaseAssistActive = false;
        m_releaseAssistConfirmationTicks = 0;
        resetReleaseTelemetry();
        resetContinuousThreat();
        m_rapidInputStreak = 0;
        return false;
    }

    void markUnsupportedSimulationInteraction() {
        if (m_simulating) {
            m_simulationUnsupportedInteraction = true;
        }
    }

    bool simulationTouchesUnsupportedInteraction(PlayerObject* player, GameObject* object) const {
        if (!m_simulating || !player || !requiresFailOpenSimulation(object)) return false;

        auto playerRect = player->getObjectRect();
        playerRect.origin.x -= 2.0f;
        playerRect.origin.y -= 2.0f;
        playerRect.size.width += 4.0f;
        playerRect.size.height += 4.0f;
        return playerRect.intersectsRect(object->getObjectRect());
    }

    bool simulationCanProcessInteraction(GameObject* object) const {
        return m_simulating && canSimulateWorldInteraction(object);
    }

    bool simulationTouchesProcessableInteraction(PlayerObject* player, GameObject* object) const {
        if (!simulationCanProcessInteraction(object) || !player) return false;
        return player->getObjectRect().intersectsRect(object->getObjectRect());
    }

    void logSimulatedWorldInteraction(GameObject* object) {
        if (!object) return;
        if (std::find(m_loggedSimulatedPads.begin(), m_loggedSimulatedPads.end(), object) !=
            m_loggedSimulatedPads.end()) {
            return;
        }
        m_loggedSimulatedPads.push_back(object);
        m_telemetrySimulatedPads++;
        predictiveTelemetryPrintf("[Telemetry][World] simulation=jump-pad object-type=%d object-id=%d\\n",
            static_cast<int>(object->m_objectType), object->m_objectID);
    }

    void logUnsupportedWorldInteraction(GameObject* object) {
        if (!object) return;
        if (std::find(m_loggedFailOpenInteractions.begin(),
            m_loggedFailOpenInteractions.end(), object) !=
            m_loggedFailOpenInteractions.end()) {
            return;
        }
        m_loggedFailOpenInteractions.push_back(object);
        m_telemetryFailOpenInteractions++;
        predictiveTelemetryPrintf("[Telemetry][World] simulation=fail-open object-type=%d object-id=%d trigger=%d\\n",
            static_cast<int>(object->m_objectType), object->m_objectID,
            object->m_isTrigger ? 1 : 0);
    }

    void setFrameDelta(float dt) {
        PlayLayer* pl = PlayLayer::get();
        if (pl && pl->m_gameState.m_timeWarp > 0.f) {
            m_frameDt = dt / pl->m_gameState.m_timeWarp;
        } else {
            m_frameDt = dt;
        }
    }

    bool shouldAssistRelease(PlayLayer* pl, PlayerObject* p1) {
        if (!pl || !p1 || m_releaseAssistActive || m_waitTicks != -1 ||
            !m_physicalHolding || !m_gameHolding) {
            return false;
        }

        const bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        const bool isWave = p1->m_isDart || (p2 && p2->m_isDart);
        const bool isRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        if (!isWave && !isRobot) return false;

        const int minimumHold = isWave
            ? WAVE_RELEASE_ASSIST_MIN_HOLD_TICKS
            : ROBOT_RELEASE_ASSIST_MIN_HOLD_TICKS;
        if (m_ticksSinceLastInput < minimumHold) {
            reportReleaseTelemetry(ReleaseTelemetryState::HoldTooShort, isWave,
                -1, -1, 0, 0);
            return false;
        }

        int baseHorizon = 0;
        int extraHorizon = 0;
        float playerSpeed = 0.f;
        const int horizon = getAdaptiveHorizon(
            p1, p2, isDual, baseHorizon, extraHorizon, playerSpeed
        );
        const int minimumGain = isWave
            ? WAVE_RELEASE_ASSIST_MIN_GAIN_TICKS
            : ROBOT_RELEASE_ASSIST_MIN_GAIN_TICKS;
        const int maximumDanger = isWave
            ? WAVE_RELEASE_ASSIST_MAX_DANGER_TICKS
            : ROBOT_RELEASE_ASSIST_MAX_DANGER_TICKS;

        bool eventBeforeApply = false;
        bool unsupportedInteraction = false;
        const int heldTicks = getSurvivalTicks(
            pl, p1, p2, isDual, 0, true, horizon,
            eventBeforeApply, unsupportedInteraction
        );
        if (unsupportedInteraction) {
            reportReleaseTelemetry(ReleaseTelemetryState::UnsupportedInteraction,
                isWave, heldTicks, -1, horizon, minimumGain);
            return false;
        }
        if (heldTicks >= horizon) {
            reportReleaseTelemetry(ReleaseTelemetryState::HoldIsSafe, isWave,
                heldTicks, -1, horizon, minimumGain);
            return false;
        }
        if (heldTicks > maximumDanger) {
            reportReleaseTelemetry(ReleaseTelemetryState::DangerTooFarAway,
                isWave, heldTicks, -1, horizon, minimumGain);
            return false;
        }

        const int releasedTicks = getSurvivalTicks(
            pl, p1, p2, isDual, 0, false, horizon,
            eventBeforeApply, unsupportedInteraction
        );
        if (unsupportedInteraction) {
            reportReleaseTelemetry(ReleaseTelemetryState::UnsupportedInteraction,
                isWave, heldTicks, releasedTicks, horizon, minimumGain);
            return false;
        }
        if (releasedTicks < horizon) {
            reportReleaseTelemetry(ReleaseTelemetryState::ReleaseDoesNotClear,
                isWave, heldTicks, releasedTicks, horizon, minimumGain);
            return false;
        }
        if (releasedTicks - heldTicks < minimumGain) {
            reportReleaseTelemetry(ReleaseTelemetryState::ReleaseGainTooSmall,
                isWave, heldTicks, releasedTicks, horizon, minimumGain);
            return false;
        }

        reportReleaseTelemetry(ReleaseTelemetryState::Confirming, isWave,
            heldTicks, releasedTicks, horizon, minimumGain);
        return true;
    }

    int getSurvivalTicks(PlayLayer* pl, PlayerObject* realP1, PlayerObject* realP2, bool isDual, int delayFrames, bool actionPush, int maxTicks, bool& eventBeforeApply, bool& unsupportedInteraction) {
        eventBeforeApply = false;
        unsupportedInteraction = false;
        if (!pl || !m_simPlayer1 || !realP1) return maxTicks;

        m_simulating = true;
        m_simulationDead = false;
        m_simulationUnsupportedInteraction = false;

        syncSimPlayer(m_simPlayer1, realP1);
        if (isDual && m_simPlayer2 && realP2) {
            syncSimPlayer(m_simPlayer2, realP2);
        }

        // Before delayFrames, maintain the pre-input button state
        if (delayFrames > 0) {
            if (!actionPush) {
                m_simPlayer1->pushButton(PlayerButton::Jump);
                if (isDual && m_simPlayer2) {
                    m_simPlayer2->pushButton(PlayerButton::Jump);
                }
            } else {
                m_simPlayer1->releaseButton(PlayerButton::Jump);
                if (isDual && m_simPlayer2) {
                    m_simPlayer2->releaseButton(PlayerButton::Jump);
                }
            }
        }

        float dt = getValidDt();
        int survived = 0;
        bool isContinuousMode = realP1->m_isDart || realP1->m_isBird || realP1->m_isSwing ||
                                (isDual && realP2 && (realP2->m_isDart || realP2->m_isBird || realP2->m_isSwing));
        int airborneTicks = 0;
        int safeLandedTicks = 0;

        for (int i = 0; i < delayFrames + maxTicks; i++) {
            if (i == delayFrames) {
                if (actionPush) {
                    m_simPlayer1->pushButton(PlayerButton::Jump);
                    if (isDual && m_simPlayer2) {
                        m_simPlayer2->pushButton(PlayerButton::Jump);
                    }
                } else {
                    m_simPlayer1->releaseButton(PlayerButton::Jump);
                    if (isDual && m_simPlayer2) {
                        m_simPlayer2->releaseButton(PlayerButton::Jump);
                    }
                }
            }

            m_simPlayer1->update(dt);
            m_simPlayer1->updateRotation(dt);
            if (isDual && m_simPlayer2) {
                m_simPlayer2->update(dt);
                m_simPlayer2->updateRotation(dt);
            }
            if (m_simulationDead || m_simulationUnsupportedInteraction) {
                eventBeforeApply = i < delayFrames;
                unsupportedInteraction = m_simulationUnsupportedInteraction;
                break;
            }

            resetCollisionLog(m_simPlayer1);
            pl->checkCollisions(m_simPlayer1, dt, false);
            if (m_simulationDead || m_simulationUnsupportedInteraction) {
                eventBeforeApply = i < delayFrames;
                unsupportedInteraction = m_simulationUnsupportedInteraction;
                break;
            }

            if (isDual && m_simPlayer2 && realP2) {
                resetCollisionLog(m_simPlayer2);
                pl->checkCollisions(m_simPlayer2, dt, false);
                if (m_simulationDead || m_simulationUnsupportedInteraction) {
                    eventBeforeApply = i < delayFrames;
                    unsupportedInteraction = m_simulationUnsupportedInteraction;
                    break;
                }
            }

            if (i >= delayFrames) {
                survived++;
                if (!isContinuousMode) {
                    bool p1Grounded = m_simPlayer1->m_isOnGround;
                    bool p2Grounded = (!isDual || !m_simPlayer2) ? true : m_simPlayer2->m_isOnGround;
                    if (!p1Grounded || !p2Grounded) {
                        airborneTicks++;
                        safeLandedTicks = 0;
                    } else if (airborneTicks >= 15) {
                        safeLandedTicks++;
                        if (safeLandedTicks >= 10) {
                            survived = maxTicks;
                            break;
                        }
                    }
                }
            }
        }

        m_simulating = false;
        return survived;
    }

    // Maintains a hidden, bounded forecast of the currently held path. This
    // never changes input by itself; it only allows a later real input to use
    // a longer horizon when an actual collision is already on the forecast.
    void updateContinuousThreat(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || !player || player != pl->m_player1 || !m_enabled ||
            player->m_isDead || m_simulating) {
            return;
        }

        const bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        if (player->m_isShip || (p2 && p2->m_isShip)) {
            resetContinuousThreat();
            return;
        }

        const bool wasThreat = m_threatTicks >= 0;
        if (m_threatTicks >= 0) {
            m_threatTicks--;
        }
        if (m_threatTicks < 0) {
            m_threatRefreshCountdown = 0;
        }
        if (m_threatRefreshCountdown > 0) {
            m_threatRefreshCountdown--;
            return;
        }

        const bool isWave = player->m_isDart || (p2 && p2->m_isDart);
        const bool isMiniWave = (player->m_isDart && player->m_vehicleSize < 1.0f) ||
                                (p2 && p2->m_isDart && p2->m_vehicleSize < 1.0f);
        const int probeHorizon = isMiniWave ? CONTINUOUS_PROBE_MINI_WAVE_TICKS :
            (isWave ? CONTINUOUS_PROBE_WAVE_TICKS : CONTINUOUS_PROBE_NORMAL_TICKS);

        bool eventBeforeApply = false;
        bool unsupportedInteraction = false;
        const int survived = getSurvivalTicks(
            pl, player, p2, isDual, 0, m_gameHolding, probeHorizon,
            eventBeforeApply, unsupportedInteraction
        );
        m_telemetryContinuousProbes++;
        m_threatHorizon = probeHorizon;
        m_threatUnsupported = unsupportedInteraction;
        m_threatTicks = (!unsupportedInteraction && survived < probeHorizon)
            ? survived : -1;

        const bool hasThreat = m_threatTicks >= 0;
        m_threatRefreshCountdown = (hasThreat &&
            m_threatTicks <= CONTINUOUS_PROBE_DANGER_TICKS)
            ? CONTINUOUS_PROBE_DANGER_REFRESH_TICKS - 1
            : CONTINUOUS_PROBE_CLEAR_REFRESH_TICKS - 1;

        if (hasThreat) {
            if (!wasThreat) {
                m_telemetryContinuousThreats++;
                predictiveTelemetryPrintf("[Telemetry][Continuous] state=threat mode=%s hit-in=%d horizon=%d holding=%d\\n",
                    isWave ? "wave" : "normal", m_threatTicks, probeHorizon,
                    m_gameHolding ? 1 : 0);
            }
        } else if (wasThreat) {
            predictiveTelemetryPrintf("[Telemetry][Continuous] state=clear horizon=%d unsupported=%d\\n",
                probeHorizon, unsupportedInteraction ? 1 : 0);
        }
    }

    bool interceptInput(GJBaseGameLayer* layer, bool push, bool isPlayer1) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl) return true;

        bool isDual = pl->m_gameState.m_isDualMode && pl->m_player2 != nullptr;
        bool isTwoPlayerControl = pl->m_level && pl->m_level->m_twoPlayerMode;

        // v4.7 leaves genuine Player 2 input alone in two-player levels.
        if (!isPlayer1 && isTwoPlayerControl) {
            return true;
        }

        // Keep the physical state and player identity paired so delayed and
        // failsafe inputs are replayed to the player that produced the event.
        m_physicalIsPlayer1 = isPlayer1;
        m_physicalHolding = push;
        m_releaseAssistActive = false;
        m_releaseAssistConfirmationTicks = 0;
        resetReleaseTelemetry();

        if (!m_enabled) {
            m_gameHolding = push;
            return true;
        }

        PlayerObject* p1 = layer->m_player1;
        PlayerObject* p2 = isDual ? pl->m_player2 : nullptr;
        if (!p1 || p1->m_isDead) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            m_releaseAssistActive = false;
            m_releaseAssistConfirmationTicks = 0;
            return true;
        }

        // 1. NEVER touch Ship mode (or Dual with Ship)
        if (p1->m_isShip || (isDual && p2 && p2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_gameHolding = push;
            m_releaseAssistActive = false;
            m_releaseAssistConfirmationTicks = 0;
            return true;
        }

        // 2. If an input is currently waiting in the buffer and the user quickly releases
        // (or re-presses) 1-2 ticks later, queue it for the next tick so fast taps are never eaten
        if (m_waitTicks >= 0) {
            if (push != m_bufferedPush) {
                m_queuedOpposite = true;
            } else {
                m_queuedOpposite = false;
            }
            m_ticksSinceLastInput = 0;
            return false;
        }

        // If a queued opposite action is due to fire on the next tick (m_waitTicks == -2)
        // and the user clicks again, fire the pending opposite action first
        if (m_waitTicks == -2) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            if (m_gameHolding != !m_bufferedPush) {
                m_SimFireButton(pl, !m_bufferedPush);
            }
        }

        bool isWave = p1->m_isDart || (isDual && p2 && p2->m_isDart);

        // 3. STREAK-BASED WAVE SPAM GUARD:
        // Only bypasses if you perform 3+ rapid inputs in a row (< 8 ticks apart).
        // Single micro-clicks and normal Wave turns around spikes/sawblades stay 100% active!
        if (m_ticksSinceLastInput < SPAM_INTERVAL_TICKS) {
            m_rapidInputStreak++;
        } else {
            m_rapidInputStreak = 0;
        }
        m_ticksSinceLastInput = 0;

        if (isWave && m_rapidInputStreak >= SPAM_STREAK_REQUIRED) {
            predictiveTelemetryPrintf("[Telemetry][Input] action=%s mode=wave decision=raw reason=spam-guard streak=%d\\n",
                push ? "PRESS" : "RELEASE", m_rapidInputStreak);
            m_gameHolding = push;
            return true;
        }

        // Only Wave (m_isDart) and Robot (m_isRobot) change trajectory on RELEASE
        bool releaseAffectsPhysics = p1->m_isDart || p1->m_isRobot ||
                                     (isDual && p2 && (p2->m_isDart || p2->m_isRobot));
        if (!push && !releaseAffectsPhysics) {
            predictiveTelemetryPrintf("[Telemetry][Input] action=RELEASE mode=normal decision=raw reason=release-does-not-affect-physics\\n");
            m_gameHolding = false;
            return true;
        }

        m_queuedOpposite = false;
        const char* act = push ? "PRESS" : "RELEASE";

        // Retain v4.7's short base horizon for responsiveness, then add a
        // bounded look-ahead only for high-speed / compact trajectories.
        int baseHorizon = 0;
        int adaptiveExtra = 0;
        float playerSpeed = 0.f;
        int targetHorizon = getAdaptiveHorizon(
            p1, p2, isDual, baseHorizon, adaptiveExtra, playerSpeed
        );
        // The ordinary input path stays short and responsive. Escalate only
        // when the continuously refreshed held-path cache has already found
        // a collision outside that normal decision window.
        const bool continuousThreatEscalation = !m_threatUnsupported &&
            m_threatTicks > targetHorizon &&
            m_threatHorizon > targetHorizon;
        if (continuousThreatEscalation) {
            targetHorizon = m_threatHorizon;
            m_telemetryContinuousEscalations++;
            predictiveTelemetryPrintf("[Telemetry][Continuous] state=escalate action=%s mode=%s hit-in=%d horizon=%d\\n",
                push ? "PRESS" : "RELEASE", isWave ? "wave" : "normal",
                m_threatTicks, targetHorizon);
        }
        m_telemetryPredictedInputs++;
        m_telemetryAdaptiveExtraTotal += adaptiveExtra;
        m_telemetryAdaptiveExtraMaximum = std::max(
            m_telemetryAdaptiveExtraMaximum, adaptiveExtra
        );
        int maxDelayFrames = isWave ? m_maxTicksWave : m_maxTicksNormal;

        // 4. Check Frame 0 (immediate input) across the full trajectory horizon
        bool eventBeforeApply = false;
        bool unsupportedInteraction = false;
        int ticksFrame0 = getSurvivalTicks(pl, p1, p2, isDual, 0, push, targetHorizon, eventBeforeApply, unsupportedInteraction);
        if (unsupportedInteraction) {
            m_telemetryUnsupportedFallbacks++;
            predictiveTelemetryPrintf("[Telemetry][Input] action=%s mode=%s horizon=%d base=%d adaptive-extra=%d speed=%.2f frame0=%d decision=raw reason=unsupported-interaction\\n",
                act, isWave ? "wave" : "normal", targetHorizon, baseHorizon,
                adaptiveExtra, playerSpeed, ticksFrame0);
            m_gameHolding = push;
            return true;
        }
        if (ticksFrame0 >= targetHorizon) {
            m_telemetryImmediateSafe++;
            predictiveTelemetryPrintf("[Telemetry][Input] action=%s mode=%s horizon=%d base=%d adaptive-extra=%d speed=%.2f frame0=%d decision=raw reason=immediate-safe\\n",
                act, isWave ? "wave" : "normal", targetHorizon, baseHorizon,
                adaptiveExtra, playerSpeed, ticksFrame0);
            m_gameHolding = push;
            return true;
        }

        // 5. Frame 0 hits a spike, sawblade, or slope within the horizon!
        // Scan +1..maxDelayFrames for the earliest delay that genuinely clears the obstacle.
        int bestDelay = 0;
        int bestTicks = ticksFrame0;

        for (int delay = 1; delay <= maxDelayFrames; delay++) {
            m_telemetryCandidatesTested++;
            int ticks = getSurvivalTicks(pl, p1, p2, isDual, delay, push, targetHorizon, eventBeforeApply, unsupportedInteraction);
            if (eventBeforeApply || unsupportedInteraction) {
                if (unsupportedInteraction) {
                    m_telemetryUnsupportedFallbacks++;
                }
                predictiveTelemetryPrintf("[Telemetry][Candidate] action=%s delay=%d survival=%d decision=raw reason=%s\\n",
                    act, delay, ticks,
                    unsupportedInteraction ? "unsupported-interaction" : "event-before-input");
                m_gameHolding = push;
                return true;
            }

            predictiveTelemetryPrintf("[Telemetry][Candidate] action=%s delay=%d survival=%d baseline=%d horizon=%d\\n",
                act, delay, ticks, ticksFrame0, targetHorizon);

            if (isWave) {
                // In Wave mode, delaying a turn inside a flat corridor naturally adds ~(2 * delay)
                // ticks against the opposite wall. Require at least (2 * delay + 10) extra ticks
                // so we ONLY buffer when the delayed trajectory actually clears past a spike,
                // sawblade, or corner!
                int minClearedGain = (delay * 2) + 10;
                if (ticks >= targetHorizon && (ticks - ticksFrame0) >= (delay * 2) + 6) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks >= ticksFrame0 + minClearedGain && ticks > bestTicks + 2) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    if (ticks >= targetHorizon) break;
                }
            } else {
                if (ticks >= targetHorizon) {
                    bestDelay = delay;
                    bestTicks = ticks;
                    break;
                }
                if (ticks > bestTicks + 12) {
                    bestDelay = delay;
                    bestTicks = ticks;
                }
            }
        }

        if (bestDelay > 0) {
            // Preserve v4.7 Wave behavior exactly: it decides from the
            // standard 38/28-tick path and does not demand a second, longer
            // confirmation pass. The confidence extension remains enabled
            // for non-Wave modes where inputs are less time-critical.
            const int confidenceHorizon = targetHorizon +
                (isWave ? 0 : BUFFER_CONFIDENCE_RUNWAY_TICKS);
            bool confidenceEventBeforeApply = false;
            bool confidenceUnsupportedInteraction = false;
            const int confidentTicks = getSurvivalTicks(
                pl, p1, p2, isDual, bestDelay, push, confidenceHorizon,
                confidenceEventBeforeApply, confidenceUnsupportedInteraction
            );

            bool toleranceEventBeforeApply = false;
            bool toleranceUnsupportedInteraction = false;
            const int laterTicks = getSurvivalTicks(
                pl, p1, p2, isDual,
                bestDelay + (isWave ? 0 : BUFFER_CONFIDENCE_TIMING_TOLERANCE_TICKS),
                push, targetHorizon, toleranceEventBeforeApply,
                toleranceUnsupportedInteraction
            );

            const bool invalidSimulation = confidenceEventBeforeApply ||
                confidenceUnsupportedInteraction || toleranceEventBeforeApply ||
                toleranceUnsupportedInteraction;
            const bool hasRunway = confidentTicks >= confidenceHorizon;
            const bool hasTimingTolerance = laterTicks >= targetHorizon;
            if (invalidSimulation || !hasRunway || !hasTimingTolerance) {
                m_telemetryConfidenceRejections++;
                if (confidenceUnsupportedInteraction || toleranceUnsupportedInteraction) {
                    m_telemetryUnsupportedFallbacks++;
                }
                predictiveTelemetryPrintf("[Telemetry][Confidence] action=%s delay=%d runway=%d/%d later=%d/%d decision=raw reason=%s\\n",
                    act, bestDelay, confidentTicks, confidenceHorizon,
                    laterTicks, targetHorizon,
                    invalidSimulation ? "unsafe-interaction-or-early-event" :
                    (!hasRunway ? "insufficient-runway" : "timing-not-tolerant"));
                m_gameHolding = push;
                return true;
            }

            m_telemetryBufferedInputs++;
            predictiveTelemetryPrintf("[Telemetry][Input] action=%s mode=%s horizon=%d base=%d adaptive-extra=%d speed=%.2f frame0=%d best-delay=%d best=%d confidence=%d/%d later=%d/%d decision=buffer\\n",
                act, isWave ? "wave" : "normal", targetHorizon, baseHorizon,
                adaptiveExtra, playerSpeed, ticksFrame0, bestDelay, bestTicks,
                confidentTicks, confidenceHorizon, laterTicks, targetHorizon);
            predictiveLogPrintf("[InputBuffer] *** SAVED +%d TICKS (%.2f frames @ 60fps) %s! (Frame0: %d -> Delayed: %d) [Limit: %d] ***\n",
                bestDelay, bestDelay / 4.0f, act, ticksFrame0, bestTicks, maxDelayFrames);
            m_bufferedPush = push;
            m_waitTicks = bestDelay;
            return false;
        }

        m_telemetryRawNoBetterDelay++;
        predictiveTelemetryPrintf("[Telemetry][Input] action=%s mode=%s horizon=%d base=%d adaptive-extra=%d speed=%.2f frame0=%d best-delay=0 best=%d decision=raw reason=no-better-delay\\n",
            act, isWave ? "wave" : "normal", targetHorizon, baseHorizon,
            adaptiveExtra, playerSpeed, ticksFrame0, bestTicks);
        m_gameHolding = push;
        return true;
    }

    void onPlayerUpdate(PlayerObject* player) {
        PlayLayer* pl = PlayLayer::get();
        if (!pl || player != pl->m_player1) return;

        if (!m_enabled) {
            if (m_releaseAssistActive && m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
            m_releaseAssistActive = false;
            m_releaseAssistConfirmationTicks = 0;
            return;
        }

        if (m_ticksSinceLastInput < 999) {
            m_ticksSinceLastInput++;
        }

        if (player->m_isShip || (pl->m_gameState.m_isDualMode && pl->m_player2 && pl->m_player2->m_isShip)) {
            m_waitTicks = -1;
            m_queuedOpposite = false;
            m_releaseAssistActive = false;
            m_releaseAssistConfirmationTicks = 0;
            if (m_gameHolding != m_physicalHolding) {
                m_SimFireButton(pl, m_physicalHolding);
            }
            return;
        }

        if (m_waitTicks == 0) {
            m_SimFireButton(pl, m_bufferedPush);
            if (m_queuedOpposite) {
                m_waitTicks = -2;
                m_queuedOpposite = false;
            } else {
                m_waitTicks = -1;
            }
        } else if (m_waitTicks > 0) {
            m_waitTicks--;
        } else if (m_waitTicks == -2) {
            m_SimFireButton(pl, !m_bufferedPush);
            m_queuedOpposite = false;
            m_waitTicks = -1;
        } else if (m_waitTicks == -1) {
            // Require the same emergency result across two rendered frames so
            // a transient prediction never takes ownership of a normal hold.
            if (!m_releaseAssistActive && shouldAssistRelease(pl, player)) {
                m_releaseAssistConfirmationTicks++;
            } else if (!m_releaseAssistActive) {
                m_releaseAssistConfirmationTicks = 0;
            }

            if (!m_releaseAssistActive &&
                m_releaseAssistConfirmationTicks >= RELEASE_ASSIST_CONFIRMATION_TICKS) {
                m_releaseAssistActive = true;
                m_releaseAssistConfirmationTicks = 0;
                m_telemetryReleaseAssists++;
                predictiveTelemetryPrintf("[Telemetry][Release] decision=assist-release reason=emergency-confirmed\\n");
                predictiveLogPrintf("[InputBuffer] *** RELEASE ASSIST ***\n");
                m_SimFireButton(pl, false);
            } else if (!m_releaseAssistActive && m_gameHolding != m_physicalHolding) {
                // FAILSAFE: When no buffer is active, guarantee the game's hold state
                // matches the physical button unless a release assist owns it.
                m_SimFireButton(pl, m_physicalHolding);
            }
        }
    }

    void m_SimFireButton(PlayLayer* pl, bool push) {
        m_firingBuffered = true;
        m_gameHolding = push;
        pl->handleButton(push, 1, m_physicalIsPlayer1);
        m_firingBuffered = false;
    }
};

static TrajectoryBufferSim s_sim;

static void refreshAssistHud() {
    auto* layer = PlayLayer::get();
    if (!layer || !layer->m_uiLayer) return;

    auto* status = static_cast<CCLabelBMFont*>(
        layer->m_uiLayer->getChildByID(ASSISTED_STATUS_NODE_ID)
    );
    auto* stage = static_cast<CCLabelBMFont*>(
        layer->m_uiLayer->getChildByID(ASSISTED_STAGE_NODE_ID)
    );
    if (!status || !stage) return;

    // Stage 5 is true raw pass-through input. All assist stages stay visibly
    // marked, while raw input hides both pieces of the indicator.
    const bool visible = s_sim.isEnabled();
    status->setVisible(visible);
    stage->setVisible(visible);
    if (!visible) return;

    stage->setString(s_sim.getActiveTickLabel());

    const auto size = CCDirector::sharedDirector()->getWinSize();
    constexpr float margin = 12.f;
    constexpr float spacing = 5.f;
    const float statusWidth = status->getContentSize().width * status->getScale();
    const float stageWidth = stage->getContentSize().width * stage->getScale();
    const float clusterWidth = statusWidth + spacing + stageWidth;

    const std::string position = Mod::get()->getSettingValue<std::string>("hud-position");
    const bool top = position.starts_with("top-");
    const bool center = position.ends_with("-center");
    const bool right = position.ends_with("-right");
    const float x = right ? size.width - margin - clusterWidth :
        center ? (size.width - clusterWidth) / 2.f : margin;
    const float y = top ? size.height - margin : margin;

    status->setPosition({ x, y });
    stage->setPosition({ x + statusWidth + spacing, y });
}

$on_mod(Loaded) {
    if (Mod::get()->getSettingValue<bool>("show-live-console")) {
        AllocConsole();
        FILE* f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleTitleA("ClickAssist - Live Console");
        predictiveLogPrintf("============================================================\n");
        predictiveLogPrintf(" [ClickAssist] LOADED — all active sessions are assisted and invalid\n");
        predictiveLogPrintf(" [ClickAssist] Detailed telemetry is controlled in Geode settings\n");
        predictiveLogPrintf("============================================================\n");
    }

    listenForKeybindSettingPresses("cycle-stage", [](Keybind const&, bool down, bool repeat, double) {
        if (down && !repeat && PlayLayer::get()) {
            s_sim.cyclePreset();
            return ListenerResult::Stop;
        }
        return ListenerResult::Propagate;
    });
    listenForSettingChanges<std::string>("hud-position", [](std::string const&) {
        refreshAssistHud();
    });
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", -1000000);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", -1000000);
        (void)self.setHookPriority("PlayLayer::flipGravity", -1000000);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        if (m_uiLayer && !m_uiLayer->getChildByID(ASSISTED_STATUS_NODE_ID)) {
            auto* status = CCLabelBMFont::create("ClickAssist", "bigFont.fnt");
            auto* stage = CCLabelBMFont::create("2/2", "bigFont.fnt");
            if (status && stage) {
                status->setID(ASSISTED_STATUS_NODE_ID);
                stage->setID(ASSISTED_STAGE_NODE_ID);
                status->setAnchorPoint({ 0.f, 0.5f });
                stage->setAnchorPoint({ 0.f, 0.5f });
                status->setScale(0.26f);
                stage->setScale(0.20f);
                status->setColor(ccc3(200, 64, 64));
                stage->setColor(ccc3(165, 165, 165));
                status->setOpacity(150);
                stage->setOpacity(135);
                m_uiLayer->addChild(status, 9999);
                m_uiLayer->addChild(stage, 9999);
            }
        }
        refreshAssistHud();
        // File I/O starts only after the game has safely created a level. It
        // avoids touching mod save paths during Geode's loader startup, while
        // still collecting every gameplay prediction decision.
        openTelemetryFile();
        s_sim.init();
        return true;
    }

    void destroyPlayer(PlayerObject* player, GameObject* gameObject) {
        if (s_sim.checkSimulationDeath(player)) return;
        PlayLayer::destroyPlayer(player, gameObject);
    }

    void playEndAnimationToPos(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        PlayLayer::playEndAnimationToPos(p0);
    }

    void flipGravity(PlayerObject* p0, bool p1, bool p2) {
        if (s_sim.isSimulating()) return;
        PlayLayer::flipGravity(p0, p1, p2);
    }

    void onQuit() {
        s_sim.quit();
        PlayLayer::onQuit();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool unk) {
        bool res = LevelEditorLayer::init(level, unk);
        s_sim.quit();
        return res;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", -1000000);
    }

    void addPoint(cocos2d::CCPoint p0) {
        if (s_sim.isSimulating()) return;
        HardStreak::addPoint(p0);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", -1000000);
    }

    void triggerObject(GJBaseGameLayer* p0, int p1, const gd::vector<int>* p2) {
        if (s_sim.isSimulating()) {
            s_sim.markUnsupportedSimulationInteraction();
            return;
        }
        EffectGameObject::triggerObject(p0, p1, p2);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", -1000000);
    }

    void playShineEffect() {
        if (s_sim.isSimulating()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", -1000000);
        (void)self.setHookPriority("PlayerObject::incrementJumps", -1000000);
        (void)self.setHookPriority("PlayerObject::ringJump", -1000000);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (s_sim.isSimulating()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (s_sim.isSimulating()) return;
        PlayerObject::incrementJumps();
    }

    void update(float dt) {
        if (!s_sim.isSimulating()) {
            s_sim.onPlayerUpdate(this);
        }
        PlayerObject::update(dt);
        if (PlayLayer::get() && !s_sim.isSimulating()) {
            s_sim.setFrameDelta(dt);
            s_sim.updateContinuousThreat(this);
        }
    }

    void ringJump(RingObject* p0, bool p1) {
        if (s_sim.isSimulating()) {
            s_sim.markUnsupportedSimulationInteraction();
            return;
        }
        PlayerObject::ringJump(p0, p1);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", -1000000);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", -1000000);
    }

    bool canBeActivatedByPlayer(PlayerObject* p0, EffectGameObject* p1) {
        if (s_sim.isSimulating()) {
            if (s_sim.simulationCanProcessInteraction(p1)) {
                return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
            }
            s_sim.markUnsupportedSimulationInteraction();
            return false;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(p0, p1);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!s_sim.isSimulating() && !s_sim.isFiringBuffered() && button == 1 && PlayLayer::get()) {
            if (!s_sim.interceptInput(this, down, isPlayer1)) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* vec, int objectsCount, float dt) {
        if (s_sim.isSimulating()) {
            gd::vector<GameObject*> extra;
            extra.reserve(objectsCount);
            for (int i = 0; i < objectsCount; i++) {
                GameObject* obj = vec->at(i);
                if (!obj) continue;
                if (s_sim.simulationTouchesProcessableInteraction(player, obj)) {
                    s_sim.logSimulatedWorldInteraction(obj);
                    extra.push_back(obj);
                    continue;
                }
                if (s_sim.simulationTouchesUnsupportedInteraction(player, obj)) {
                    s_sim.logUnsupportedWorldInteraction(obj);
                    s_sim.markUnsupportedSimulationInteraction();
                    continue;
                }

                // `vec` is already GD's nearby collision-candidate list.
                // Preserve all ordinary candidates and let GD resolve their
                // exact collision geometry. Filtering by broad type drops
                // several custom saws, angled hazards, and slope variants.
                // World-changing objects are removed above through the
                // fail-open guard, so this remains isolated from the level.
                extra.push_back(obj);
            }
            GJBaseGameLayer::collisionCheckObjects(player, &extra, extra.size(), dt);
            return;
        }
        GJBaseGameLayer::collisionCheckObjects(player, vec, objectsCount, dt);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (s_sim.isSimulating()) {
            s_sim.markUnsupportedSimulationInteraction();
            return;
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }
};
