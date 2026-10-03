// xr-sim: the control channel.
//
//   <dir>\command.txt   agent -> sim, one command per line
//   <dir>\ack.txt       sim -> agent, the last applied batch and the frame it landed on
//   <dir>\state.json    sim -> agent, written atomically
//
// Two properties keep the control channel deterministic:
//
//  1. It polls at 50 Hz on a DEDICATED THREAD, not once per frame. In step mode
//     xrWaitFrame is blocked, so a frame-path poller could never receive the
//     `step` that unblocks it - the sim would deadlock by construction.
//
//  2. It is PARSE-THEN-COMMIT. Lines are staged under a lock and swapped in at
//     one point inside xrWaitFrame, so a multi-line file lands as a single
//     instantaneous rig change and never tears across a frame.
//
// It also discards a command.txt older than its own start time. That is the same
// stale-file trap: a leftover `head rot 90 0 0` re-applying at boot looks
// exactly like a camera bug.
//
// And it marks a write as seen only once it has read it, so a file it could not
// open, or caught empty mid-rewrite, is retried on the next poll rather than
// lost (see poll_once).

#include "xrsim_internal.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <string>
#include <thread>
#include <vector>

namespace xrsim {
namespace {

std::mutex g_pendingMutex;
Rig g_staging;
bool g_dirty = false;

std::thread g_thread;
std::atomic<bool> g_running{false};
FILETIME g_lastWrite{};
uint64_t g_startMs = 0;
uint64_t g_lastStateWriteMs = 0;
uint32_t g_appliedSeq = 0;
uint32_t g_ackFrame = 0;
char g_ackText[512] = {};

// Timed holds. Evaluated at the commit point so a `btn a press 150` releases on
// a frame boundary rather than whenever the control thread happens to tick.
struct Hold {
    bool active = false;
    bool byFrames = false;
    uint64_t untilMs = 0;
    uint64_t untilFrame = 0;
};
Hold g_holds[VC_COUNT];

// Smooth motion for `head to` / `hand to`.
//
// Smooth hand motion is not a convenience: the mod derives wrench-swing SPEED
// from successive XR hand samples (openxr_input.cpp publish_sample), so a hand
// that teleports between poses cannot produce a swing a user would recognise.
// Driving a real gesture needs interpolation across frames.
struct Motion {
    bool active = false;
    Pose from = pose_identity();
    Pose to = pose_identity();
    uint64_t startMs = 0;
    uint32_t durMs = 0;
    bool linear = false;
    bool isAim = false;   // hand motions only: which pose slot is being driven
};
Motion g_headMotion;
Motion g_handMotion[2];

// `head orbit`
struct Orbit {
    bool active = false;
    float degPerSec = 0.0f;
    uint64_t startMs = 0;
    uint32_t durMs = 0;
    float baseYaw = 0.0f;
};
Orbit g_orbit;

void path_in_dir(wchar_t* out, size_t n, const wchar_t* leaf) {
    swprintf_s(out, n, L"%s\\%s", log::dir(), leaf);
}

// Escape a string for a JSON value. Windows paths are the reason this exists:
// an unescaped "C:\Users\..." makes the whole file invalid JSON, and every
// script that reads state.json then fails with a parse error that looks like a
// torn read rather than what it is. Caught by the first PowerShell reader.
const char* json_escape(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < cap; ++i) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        switch (c) {
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                o += sprintf_s(out + o, cap - o, "\\u%04X", c);
            } else {
                out[o++] = static_cast<char>(c);
            }
            break;
        }
    }
    out[o] = '\0';
    return out;
}

void set_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(g.lastCmdError, _TRUNCATE, fmt, args);
    va_end(args);
    g.errors.fetch_add(1);
    XRSIM_LOG("xrsim: command error - %s", g.lastCmdError);
}

// --- tiny tokenizer --------------------------------------------------------
struct Args {
    char buf[256];
    char* tok[12];
    int n = 0;

    void parse(const char* line) {
        strncpy_s(buf, line, _TRUNCATE);
        n = 0;
        char* ctx = nullptr;
        char* t = strtok_s(buf, " \t\r\n", &ctx);
        while (t && n < 12) {
            tok[n++] = t;
            t = strtok_s(nullptr, " \t\r\n", &ctx);
        }
    }
    const char* s(int i) const { return (i < n) ? tok[i] : ""; }
    bool is(int i, const char* v) const { return i < n && _stricmp(tok[i], v) == 0; }
    float f(int i, float def = 0.0f) const { return (i < n) ? static_cast<float>(atof(tok[i])) : def; }
    uint32_t u(int i, uint32_t def = 0) const {
        return (i < n) ? static_cast<uint32_t>(strtoul(tok[i], nullptr, 10)) : def;
    }
};

int hand_arg(const Args& a, int i) {
    if (a.is(i, "l") || a.is(i, "left")) return 0;
    if (a.is(i, "r") || a.is(i, "right")) return 1;
    return -1;
}

VirtualControl button_by_name(const char* s) {
    if (_stricmp(s, "a") == 0) return VC_BTN_A;
    if (_stricmp(s, "b") == 0) return VC_BTN_B;
    if (_stricmp(s, "x") == 0) return VC_BTN_X;
    if (_stricmp(s, "y") == 0) return VC_BTN_Y;
    if (_stricmp(s, "menu") == 0) return VC_MENU;
    return VC_NONE;
}

void set_control(Rig& rig, VirtualControl c, bool down) {
    switch (c) {
    case VC_BTN_A: rig.btnA = down; break;
    case VC_BTN_B: rig.btnB = down; break;
    case VC_BTN_X: rig.btnX = down; break;
    case VC_BTN_Y: rig.btnY = down; break;
    case VC_MENU: rig.menu = down; break;
    case VC_CLICK_L: rig.click[0] = down; break;
    case VC_CLICK_R: rig.click[1] = down; break;
    case VC_REST_L: rig.rest[0] = down; break;
    case VC_REST_R: rig.rest[1] = down; break;
    default: break;
    }
}

void arm_hold(VirtualControl c, const Args& a, int msArgIndex, uint32_t defaultMs) {
    Hold& h = g_holds[c];
    h.active = true;
    const char* spec = a.s(msArgIndex);
    if (spec[0]) {
        const size_t len = strlen(spec);
        // A trailing 'f' means FRAMES, which is the deterministic form: under
        // `pace step` a millisecond duration has no fixed relationship to how
        // many frames the game actually got.
        if (spec[len - 1] == 'f' || spec[len - 1] == 'F') {
            h.byFrames = true;
            h.untilFrame = snapshot().index + strtoul(spec, nullptr, 10);
            return;
        }
    }
    h.byFrames = false;
    h.untilMs = now_ms() + (spec[0] ? strtoul(spec, nullptr, 10) : defaultMs);
}

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------

void apply_line(const char* line) {
    Args a;
    a.parse(line);
    if (a.n == 0 || a.s(0)[0] == '#') return;

    strncpy_s(g.lastCmd, line, _TRUNCATE);
    Rig& rig = g_staging;
    g_dirty = true;

    // --- head ---------------------------------------------------------------
    if (a.is(0, "head")) {
        if (a.is(1, "pos")) {
            rig.head.p = v3(a.f(2), a.f(3), a.f(4));
        } else if (a.is(1, "rot")) {
            rig.head.q = quat_from_ypr(deg2rad(a.f(2)), deg2rad(a.f(3)), deg2rad(a.f(4)));
        } else if (a.is(1, "pose")) {
            rig.head.p = v3(a.f(2), a.f(3), a.f(4));
            rig.head.q = quat_from_ypr(deg2rad(a.f(5)), deg2rad(a.f(6)), deg2rad(a.f(7)));
        } else if (a.is(1, "move")) {
            rig.head.p = v3_add(rig.head.p, v3(a.f(2), a.f(3), a.f(4)));
        } else if (a.is(1, "movelocal")) {
            // forward, right, up in the head's own frame
            const Vec3 local = v3(a.f(3), a.f(4), -a.f(2));
            rig.head.p = v3_add(rig.head.p, quat_rotate(rig.head.q, local));
        } else if (a.is(1, "turn")) {
            float y, p, r;
            quat_to_ypr(rig.head.q, y, p, r);
            rig.head.q = quat_from_ypr(y + deg2rad(a.f(2)), p + deg2rad(a.f(3)), r + deg2rad(a.f(4)));
        } else if (a.is(1, "height")) {
            rig.head.p.y = a.f(2, 1.6f);
        } else if (a.is(1, "valid")) {
            rig.headValid = !a.is(2, "off");
        } else if (a.is(1, "to")) {
            g_headMotion.active = true;
            g_headMotion.from = rig.head;
            g_headMotion.to.p = v3(a.f(2), a.f(3), a.f(4));
            g_headMotion.to.q = quat_from_ypr(deg2rad(a.f(5)), deg2rad(a.f(6)), deg2rad(a.f(7)));
            g_headMotion.startMs = now_ms();
            g_headMotion.durMs = a.u(8, 500);
        } else if (a.is(1, "orbit")) {
            g_orbit.active = true;
            g_orbit.degPerSec = a.f(2, 30.0f);
            g_orbit.durMs = a.u(3, 4000);
            g_orbit.startMs = now_ms();
            float y, p, r;
            quat_to_ypr(rig.head.q, y, p, r);
            g_orbit.baseYaw = rad2deg(y);
        } else {
            set_error("unknown head subcommand '%s'", a.s(1));
        }
        return;
    }

    // --- hands --------------------------------------------------------------
    if (a.is(0, "hand")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("hand needs l or r, got '%s'", a.s(1)); return; }

        if (a.is(2, "grip") || a.is(2, "aim")) {
            const bool isAim = a.is(2, "aim");
            Pose& target = isAim ? rig.aim[h] : rig.grip[h];
            if (a.is(3, "pos")) {
                target.p = v3(a.f(4), a.f(5), a.f(6));
                rig.handFollowsHead[h] = false;
            } else if (a.is(3, "rot")) {
                target.q = quat_from_ypr(deg2rad(a.f(4)), deg2rad(a.f(5)), deg2rad(a.f(6)));
                rig.handFollowsHead[h] = false;
            } else if (a.is(3, "pose")) {
                target.p = v3(a.f(4), a.f(5), a.f(6));
                target.q = quat_from_ypr(deg2rad(a.f(7)), deg2rad(a.f(8)), deg2rad(a.f(9)));
                rig.handFollowsHead[h] = false;
            } else {
                set_error("hand %s needs pos, rot or pose", a.s(2));
            }
        } else if (a.is(2, "follow")) {
            rig.handFollowsHead[h] = !a.is(4, "off") && !a.is(3, "off");
        } else if (a.is(2, "offset")) {
            // forward, right, up - the order the mod's own vrhands pos uses
            rig.handOffset[h] = v3(a.f(4), a.f(5), -a.f(3));
        } else if (a.is(2, "aimtrim")) {
            rig.aimTrimPitch[h] = a.f(3);
            rig.aimTrimYaw[h] = a.f(4);
        } else if (a.is(2, "point")) {
            // Convenience: aim relative to where the head is looking.
            rig.handFollowsHead[h] = true;
            rig.aimTrimYaw[h] = a.f(3);
            rig.aimTrimPitch[h] = a.f(4);
        } else if (a.is(2, "valid")) {
            rig.handValid[h] = !a.is(3, "off");
        } else if (a.is(2, "to")) {
            // hand <h> to grip|aim <x> <y> <z> <yaw> <pitch> <roll> <ms>
            //
            // A smooth sweep, which is what the coupling tests need: to tell
            // whether a hand or weapon MODEL is following the controller you
            // have to move the controller continuously and watch the model over
            // several frames. A teleport between two poses cannot show that.
            const bool isAim = a.is(3, "aim");
            Motion& m = g_handMotion[h];
            m.active = true;
            m.isAim = isAim;
            m.from = isAim ? rig.aim[h] : rig.grip[h];
            if (rig.handFollowsHead[h]) {
                // Detach from the head first, or the follow logic would
                // overwrite the interpolated pose every frame.
                Pose parked;
                parked.q = rig.head.q;
                parked.p = v3_add(rig.head.p, quat_rotate(rig.head.q, rig.handOffset[h]));
                m.from = parked;
                rig.grip[h] = parked;
                rig.aim[h] = parked;
                rig.handFollowsHead[h] = false;
            }
            m.to.p = v3(a.f(4), a.f(5), a.f(6));
            m.to.q = quat_from_ypr(deg2rad(a.f(7)), deg2rad(a.f(8)), deg2rad(a.f(9)));
            m.startMs = now_ms();
            m.durMs = a.u(10, 500);
        } else {
            set_error("unknown hand subcommand '%s'", a.s(2));
        }
        return;
    }
    if (a.is(0, "hands") && a.is(1, "reset")) {
        Rig fresh;
        rig_defaults(fresh);
        for (int h = 0; h < 2; ++h) {
            rig.grip[h] = fresh.grip[h];
            rig.aim[h] = fresh.aim[h];
            rig.handValid[h] = true;
            rig.handFollowsHead[h] = true;
            rig.handOffset[h] = fresh.handOffset[h];
            rig.aimTrimPitch[h] = fresh.aimTrimPitch[h];
            rig.aimTrimYaw[h] = fresh.aimTrimYaw[h];
        }
        return;
    }

    // --- buttons and axes ---------------------------------------------------
    if (a.is(0, "btn")) {
        const VirtualControl c = button_by_name(a.s(1));
        if (c == VC_NONE) { set_error("unknown button '%s'", a.s(1)); return; }
        if (a.is(2, "down")) { set_control(rig, c, true); g_holds[c].active = false; }
        else if (a.is(2, "up")) { set_control(rig, c, false); g_holds[c].active = false; }
        else if (a.is(2, "press") || a.is(2, "click")) { set_control(rig, c, true); arm_hold(c, a, 3, 150); }
        else set_error("btn needs down, up or press");
        return;
    }
    if (a.is(0, "click")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("click needs l or r"); return; }
        const VirtualControl c = (h == 0) ? VC_CLICK_L : VC_CLICK_R;
        if (a.is(2, "down")) { set_control(rig, c, true); g_holds[c].active = false; }
        else if (a.is(2, "up")) { set_control(rig, c, false); g_holds[c].active = false; }
        else { set_control(rig, c, true); arm_hold(c, a, 3, 150); }
        return;
    }
    if (a.is(0, "thumbrest")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("thumbrest needs l or r"); return; }
        rig.rest[h] = !a.is(2, "off") && !a.is(2, "release");
        return;
    }
    if (a.is(0, "trigger") || a.is(0, "grip")) {
        const bool isGrip = a.is(0, "grip");
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("%s needs l or r", a.s(0)); return; }
        float* slot = isGrip ? &rig.squeeze[h] : &rig.trigger[h];
        if (a.is(2, "pull") || a.is(2, "squeeze")) {
            *slot = 1.0f;
            const VirtualControl c = isGrip ? (h == 0 ? VC_SQUEEZE_L : VC_SQUEEZE_R)
                                            : (h == 0 ? VC_TRIGGER_L : VC_TRIGGER_R);
            arm_hold(c, a, 3, 200);
        } else {
            float v = a.f(2);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            *slot = v;
        }
        return;
    }
    if (a.is(0, "stick")) {
        const int h = hand_arg(a, 1);
        if (h < 0) { set_error("stick needs l or r"); return; }
        if (a.is(2, "center")) {
            rig.stick[h][0] = rig.stick[h][1] = 0.0f;
        } else {
            rig.stick[h][0] = a.f(2);
            rig.stick[h][1] = a.f(3);
        }
        return;
    }
    if (a.is(0, "input") && a.is(1, "clear")) {
        rig.btnA = rig.btnB = rig.btnX = rig.btnY = rig.menu = false;
        for (int h = 0; h < 2; ++h) {
            rig.click[h] = rig.rest[h] = false;
            rig.trigger[h] = rig.squeeze[h] = 0.0f;
            rig.stick[h][0] = rig.stick[h][1] = 0.0f;
        }
        for (auto& hold : g_holds) hold.active = false;
        return;
    }

    // --- pacing -------------------------------------------------------------
    if (a.is(0, "pace")) {
        if (a.is(1, "free")) { g.pacing.mode = PaceMode::Free; if (a.n > 2) g.pacing.hz = a.f(2, 90.0f); }
        else if (a.is(1, "step")) g.pacing.mode = PaceMode::Step;
        else if (a.is(1, "turbo")) g.pacing.mode = PaceMode::Turbo;
        else if (a.is(1, "timeout")) g.pacing.starveMs = a.u(2, kStepStarveMsDefault);
        else if (a.is(1, "onstarve")) g.pacing.starveAdvance = !a.is(2, "hold");
        else set_error("unknown pace mode '%s'", a.s(1));
        pacing_wake();
        return;
    }
    if (a.is(0, "step")) {
        if (a.is(1, "on")) { g.pacing.mode = PaceMode::Step; return; }
        if (a.is(1, "off")) { g.pacing.mode = PaceMode::Free; pacing_wake(); return; }
        if (a.is(1, "timeout")) { g.pacing.starveMs = a.u(2, kStepStarveMsDefault); return; }
        if (a.is(1, "onstarve")) { g.pacing.starveAdvance = !a.is(2, "hold"); return; }
        pacing_grant(a.n > 1 ? a.u(1, 1) : 1);
        return;
    }
    if (a.is(0, "refresh")) {
        const float hz = a.f(1, 90.0f);
        if (hz < 1.0f || hz > 1000.0f) { set_error("refresh out of range: %s", a.s(1)); return; }
        g.pacing.hz = hz;
        return;
    }
    if (a.is(0, "idle")) {
        if (a.is(1, "off")) g.pacing.idleBlockMs = 0;
        else if (a.is(1, "max")) g.pacing.idleMaxMs = a.u(2, kIdleMaxMsDefault);
        else if (a.is(1, "on")) g.pacing.idleBlockMs = a.u(2, 5000);
        else set_error("idle needs on, off or max");
        return;
    }

    // --- session state and hazards -----------------------------------------
    if (a.is(0, "state")) {
        if (a.is(1, "hz")) { g.stateWriteHz.store(a.u(2, 20)); return; }
        XrSessionState s = XR_SESSION_STATE_UNKNOWN;
        if (a.is(1, "ready")) s = XR_SESSION_STATE_READY;
        else if (a.is(1, "synchronized")) s = XR_SESSION_STATE_SYNCHRONIZED;
        else if (a.is(1, "visible")) s = XR_SESSION_STATE_VISIBLE;
        else if (a.is(1, "focused")) s = XR_SESSION_STATE_FOCUSED;
        else if (a.is(1, "stopping")) s = XR_SESSION_STATE_STOPPING;
        else if (a.is(1, "exiting")) s = XR_SESSION_STATE_EXITING;
        else if (a.is(1, "lost")) s = XR_SESSION_STATE_LOSS_PENDING;
        else if (a.is(1, "idle")) s = XR_SESSION_STATE_IDLE;
        if (s == XR_SESSION_STATE_UNKNOWN) { set_error("unknown session state '%s'", a.s(1)); return; }
        if (s == XR_SESSION_STATE_VISIBLE) session_focus_lose(0);
        else session_force_state(s);
        return;
    }
    if (a.is(0, "focus")) {
        if (a.is(1, "lose")) session_focus_lose(a.u(2, 0));
        else if (a.is(1, "regain")) session_force_state(XR_SESSION_STATE_FOCUSED);
        else if (a.is(1, "policy")) {
            if (a.is(2, "permissive")) g.focusPolicy = FocusPolicy::Permissive;
            else if (a.is(2, "vdxr-layers") || a.is(2, "layers"))
                g.focusPolicy = FocusPolicy::VdxrLayers;
            else g.focusPolicy = FocusPolicy::Vdxr;
        }
        else if (a.is(1, "frames")) g.focusFrames = a.u(2, 3);
        // Session 54, the VDXR park model (see FocusPolicy in xrsim_common.h).
        else if (a.is(1, "norender")) g.focusNoRender = !a.is(2, "off");
        else if (a.is(1, "throttle")) g.focusThrottleMs = a.u(2, 0);
        else set_error("focus needs lose, regain, policy, frames, norender or throttle");
        return;
    }
    if (a.is(0, "hazard")) {
        if (a.is(1, "nosystem")) g.hazards.noSystem = !a.is(2, "off");
        else if (a.is(1, "waitfail")) g.hazards.waitFail = a.u(2, 1);
        else if (a.is(1, "beginfail")) g.hazards.beginFail = a.u(2, 1);
        else if (a.is(1, "endfail")) g.hazards.endFail = a.u(2, 1);
        else if (a.is(1, "swapchainfail")) g.hazards.swapchainFail = !a.is(2, "off");
        else if (a.is(1, "attachfail")) g.hazards.attachFail = !a.is(2, "off");
        else if (a.is(1, "clear")) g.hazards = Hazards{};
        else set_error("unknown hazard '%s'", a.s(1));
        return;
    }
    if (a.is(0, "instanceloss")) {
        XrEventDataBuffer buf{};
        auto* ev = reinterpret_cast<XrEventDataInstanceLossPending*>(&buf);
        ev->type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
        ev->lossTime = now_xr_time();
        queue_event(buf);
        return;
    }

    // --- optics -------------------------------------------------------------
    if (a.is(0, "ipd")) {
        rig.ipdM = a.f(1, 63.0f) / 1000.0f;
        return;
    }
    if (a.is(0, "worldscale")) {
        rig.worldScale = a.f(1, 1.0f);
        return;
    }
    if (a.is(0, "fov")) {
        if (a.is(1, "quest3")) {
            rig.fov[0] = quest3_fov(0);
            rig.fov[1] = quest3_fov(1);
        } else if (a.is(1, "profile")) {
            rig.fov[0] = g.headsetFov[0];
            rig.fov[1] = g.headsetFov[1];
        } else if (a.is(1, "eye")) {
            const int h = hand_arg(a, 2);
            if (h < 0) { set_error("fov eye needs l or r"); return; }
            const Fov f{deg2rad(a.f(3)), deg2rad(a.f(4)), deg2rad(a.f(5)), deg2rad(a.f(6))};
            // Refuse a zero-extent fov rather than accept it and hand back black
            // captures that read as a mod bug.
            if (fov_is_degenerate(f)) {
                set_error("fov eye %s has zero extent (l=%s r=%s u=%s d=%s) - refused",
                          a.s(2), a.s(3), a.s(4), a.s(5));
                return;
            }
            rig.fov[h] = f;
        } else {
            // Symmetric-outer shorthand: `fov 54 55` gives the mod's own
            // half-angle log line exactly h=54.0 v=55.0. Argument fallbacks
            // track the built-in pinned VDXR values (session 37), not a custom
            // boot profile; use `fov profile` to restore that profile.
            const float hh = deg2rad(a.f(1, 54.0f));
            const float hv = deg2rad(a.f(2, 55.0f));
            const float inner = deg2rad(a.f(3, 44.0f));
            rig.fov[0] = Fov{-hh, inner, hv, -hv};
            rig.fov[1] = Fov{-inner, hh, hv, -hv};
        }
        return;
    }
    if (a.is(0, "recenter")) {
        recenter_local_space();
        return;
    }

    // --- capture ------------------------------------------------------------
    if (a.is(0, "capture")) {
        if (a.is(1, "next")) g.captureCountdown.store(a.u(2, 1));
        else if (a.is(1, "every")) g.captureEvery.store(a.u(2, 0));
        else if (a.is(1, "off")) { g.captureCountdown.store(0); g.captureEvery.store(0); }
        else if (a.is(1, "layers")) g.captureLayers.store(!a.is(2, "off"));
        else if (a.is(1, "size")) { g.captureWidth.store(a.u(2, 1032)); g.captureHeight.store(a.u(3, 1104)); }
        else if (a.is(1, "tag")) strncpy_s(g.captureTag, a.s(2), _TRUNCATE);
        else set_error("unknown capture subcommand '%s'", a.s(1));
        return;
    }
    if (a.is(0, "shot")) {
        if (a.n > 1) strncpy_s(g.captureTag, a.s(1), _TRUNCATE);
        g.captureCountdown.store(1);
        return;
    }
    if (a.is(0, "compose")) {
        g.composeAlways.store(a.is(1, "always"));
        return;
    }

    // --- misc ---------------------------------------------------------------
    if (a.is(0, "runtimename")) { strncpy_s(g.runtimeName, a.s(1), _TRUNCATE); return; }
    if (a.is(0, "systemname")) { strncpy_s(g.systemName, a.s(1), _TRUNCATE); return; }
    if (a.is(0, "profile")) { actions_set_profile(a.is(1, "simple")); return; }
    if (a.is(0, "log")) { XRSIM_LOG("xrsim: MARK %s", line + 3); return; }
    if (a.is(0, "note")) { return; }
    if (a.is(0, "reset")) {
        rig_defaults(g_staging);
        for (auto& h : g_holds) h.active = false;
        g_headMotion.active = false;
        g_handMotion[0].active = g_handMotion[1].active = false;
        g_orbit.active = false;
        g.pacing.mode = PaceMode::Free;
        g.pacing.hz = 90.0;
        g.pacing.idleBlockMs = 0;
        g.hazards = Hazards{};
        // Session 54: the VDXR park model resets with the script, so one .xrs
        // cannot leak its focus behaviour into the next.
        g.focusPolicy = FocusPolicy::Vdxr;
        g.focusFrames = 3;
        g.focusNoRender = false;
        g.focusThrottleMs = 0;
        g.captureCountdown.store(0);
        g.captureEvery.store(0);
        g.captureLayers.store(true);
        pacing_wake();
        return;
    }
    if (a.is(0, "status")) {
        XRSIM_LOG("xrsim: status frame=%llu state=%s pace=%d hz=%.1f credits=%u",
                  static_cast<unsigned long long>(snapshot().index),
                  session_state_name(current_session_state()), static_cast<int>(g.pacing.mode),
                  g.pacing.hz, g.pacing.credits);
        return;
    }

    set_error("unknown command '%s'", a.s(0));
}

// ---------------------------------------------------------------------------
// state.json
// ---------------------------------------------------------------------------

// Written from BOTH the frame path and the control thread, so it needs its own
// lock: two writers sharing one temp filename would interleave and the atomic
// replace would publish a mixture.
std::mutex g_stateWriteMutex;

void write_state_json() {
    std::lock_guard<std::mutex> writeLock(g_stateWriteMutex);

    wchar_t tmp[MAX_PATH], dst[MAX_PATH];
    path_in_dir(tmp, MAX_PATH, L"state.json.tmp");
    path_in_dir(dst, MAX_PATH, L"state.json");

    FILE* f = _wfsopen(tmp, L"w", _SH_DENYNO);
    if (!f) return;

    FrameSnapshot snap;
    snapshot_copy(snap);
    const Rig& r = snap.rig;
    float hy, hp, hr;
    quat_to_ypr(snap.headWorld.q, hy, hp, hr);

    char escName[512];
    fprintf(f, "{\n");
    fprintf(f, "  \"pid\": %lu,\n", GetCurrentProcessId());
    fprintf(f, "  \"runtime\": \"%s\",\n", json_escape(g.runtimeName, escName, sizeof(escName)));
    fprintf(f, "  \"graphics\": \"%s\",\n", graphics_api_name());
    fprintf(f, "  \"system\": \"%s\",\n", json_escape(g.systemName, escName, sizeof(escName)));
    fprintf(f, "  \"headsetConfig\": \"%s\",\n",
            json_escape(g.headsetConfigSource, escName, sizeof(escName)));
    fprintf(f, "  \"recommendedViews\": [{\"width\": %u, \"height\": %u, \"maxWidth\": %u, \"maxHeight\": %u}, "
               "{\"width\": %u, \"height\": %u, \"maxWidth\": %u, \"maxHeight\": %u}],\n",
            g.recommendedWidth[0], g.recommendedHeight[0], g.maxWidth[0], g.maxHeight[0],
            g.recommendedWidth[1], g.recommendedHeight[1], g.maxWidth[1], g.maxHeight[1]);
    fprintf(f, "  \"uptimeMs\": %llu,\n", static_cast<unsigned long long>(now_ms() - g_startMs));
    fprintf(f, "  \"frame\": %llu,\n", static_cast<unsigned long long>(snap.index));
    fprintf(f, "  \"waitFrames\": %llu,\n", static_cast<unsigned long long>(g_gate.waited.load()));
    fprintf(f, "  \"beginFrames\": %llu,\n", static_cast<unsigned long long>(g_gate.begun.load()));
    fprintf(f, "  \"endFrames\": %llu,\n", static_cast<unsigned long long>(g_gate.ended.load()));
    fprintf(f, "  \"frameOpen\": %s,\n", g_gate.open.load() ? "true" : "false");
    fprintf(f, "  \"framesDiscarded\": %u,\n", g_gate.discarded.load());
    fprintf(f, "  \"endsOutOfOrder\": %u,\n", g_gate.outOfOrder.load());
    fprintf(f, "  \"sessionState\": \"%s\",\n", session_state_name(snap.state));
    fprintf(f, "  \"sessionRunning\": %s,\n", session_is_running() ? "true" : "false");
    fprintf(f, "  \"actionsAttached\": %s,\n", actions_attached() ? "true" : "false");
    fprintf(f, "  \"focusPolicy\": \"%s\",\n",
            g.focusPolicy == FocusPolicy::Vdxr
                ? "vdxr"
                : (g.focusPolicy == FocusPolicy::VdxrLayers ? "vdxr-layers" : "permissive"));
    fprintf(f, "  \"focusNoRender\": %s,\n", g.focusNoRender ? "true" : "false");
    fprintf(f, "  \"focusThrottleMs\": %u,\n", g.focusThrottleMs);
    fprintf(f, "  \"layeredFrames\": %llu,\n",
            static_cast<unsigned long long>(session_layered_frames()));
    fprintf(f, "  \"paceMode\": \"%s\",\n",
            g.pacing.mode == PaceMode::Step ? "step"
                                            : (g.pacing.mode == PaceMode::Turbo ? "turbo" : "free"));
    fprintf(f, "  \"refreshHz\": %.2f,\n", g.pacing.hz);
    fprintf(f, "  \"stepsPending\": %u,\n", g.pacing.credits);
    fprintf(f, "  \"idleBlockMs\": %u,\n", g.pacing.idleBlockMs);
    fprintf(f, "  \"eventsDropped\": %u,\n", events_dropped());
    fprintf(f, "  \"ipdMm\": %.2f,\n", r.ipdM * 1000.0f);
    fprintf(f, "  \"fovDeg\": {\"l\": %.2f, \"r\": %.2f, \"u\": %.2f, \"d\": %.2f},\n",
            rad2deg(r.fov[0].angleLeft), rad2deg(r.fov[0].angleRight), rad2deg(r.fov[0].angleUp),
            rad2deg(r.fov[0].angleDown));
    fprintf(f, "  \"head\": {\"pos\": [%.4f, %.4f, %.4f], \"ypr\": [%.2f, %.2f, %.2f], \"valid\": %s},\n",
            snap.headWorld.p.x, snap.headWorld.p.y, snap.headWorld.p.z, rad2deg(hy), rad2deg(hp),
            rad2deg(hr), r.headValid ? "true" : "false");

    for (int h = 0; h < 2; ++h) {
        float y, p, rr;
        quat_to_ypr(snap.gripWorld[h].q, y, p, rr);
        fprintf(f, "  \"hand%s\": {\"pos\": [%.4f, %.4f, %.4f], \"ypr\": [%.2f, %.2f, %.2f], "
                   "\"valid\": %s, \"followsHead\": %s},\n",
                h == 0 ? "L" : "R", snap.gripWorld[h].p.x, snap.gripWorld[h].p.y,
                snap.gripWorld[h].p.z, rad2deg(y), rad2deg(p), rad2deg(rr),
                r.handValid[h] ? "true" : "false", r.handFollowsHead[h] ? "true" : "false");
    }

    fprintf(f, "  \"controls\": {\"a\": %s, \"b\": %s, \"x\": %s, \"y\": %s, \"menu\": %s, "
               "\"clickL\": %s, \"clickR\": %s, \"restL\": %s, \"restR\": %s, "
               "\"trigL\": %.3f, \"trigR\": %.3f, \"gripL\": %.3f, \"gripR\": %.3f, "
               "\"stickL\": [%.3f, %.3f], \"stickR\": [%.3f, %.3f]},\n",
            r.btnA ? "true" : "false", r.btnB ? "true" : "false", r.btnX ? "true" : "false",
            r.btnY ? "true" : "false", r.menu ? "true" : "false", r.click[0] ? "true" : "false",
            r.click[1] ? "true" : "false", r.rest[0] ? "true" : "false",
            r.rest[1] ? "true" : "false", r.trigger[0], r.trigger[1], r.squeeze[0], r.squeeze[1],
            r.stick[0][0], r.stick[0][1], r.stick[1][0], r.stick[1][1]);

    fprintf(f, "  \"layersLastFrame\": %u,\n", compositor_last_layer_count());
    fprintf(f, "  \"projectionViews\": %u,\n", compositor_last_projection_views());
    char esc[1024];
    fprintf(f, "  \"captureSeq\": %u,\n", g.captureSeq.load());
    fprintf(f, "  \"lastCapture\": \"%s\",\n", json_escape(g.lastCapturePath, esc, sizeof(esc)));
    fprintf(f, "  \"cmdSeq\": %u,\n", g.cmdSeq.load());
    fprintf(f, "  \"lastCmd\": \"%s\",\n", json_escape(g.lastCmd, esc, sizeof(esc)));
    fprintf(f, "  \"lastCmdError\": \"%s\",\n", json_escape(g.lastCmdError, esc, sizeof(esc)));
    fprintf(f, "  \"errors\": %u\n", g.errors.load());
    fprintf(f, "}\n");
    fclose(f);

    // Atomic replace, or a reader eventually catches a half-written file and the
    // script layer reports a torn-JSON error that is really a writer bug.
    MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING);
}

void write_ack() {
    wchar_t path[MAX_PATH];
    path_in_dir(path, MAX_PATH, L"ack.txt");
    FILE* f = _wfsopen(path, L"w", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "seq=%u frame=%u error=%s\nlast=%s\n", g.cmdSeq.load(), g_ackFrame,
            g.lastCmdError[0] ? g.lastCmdError : "-", g_ackText);
    fclose(f);
}

// ---------------------------------------------------------------------------
// The poll thread
// ---------------------------------------------------------------------------

// A command.txt write time is consumed only once that file has been opened and
// read and was not empty. It used to be consumed BEFORE the open, so a single
// failed open lost the command for good: every later poll saw a write time it
// had already marked seen. Two ways to get there (2026-10-03):
//
//  - A rename-over writer (Python os.replace = MoveFileExW REPLACE_EXISTING, the
//    Sims4VR driver) renames its temp file through a handle opened with DELETE
//    access and closes that handle a moment later. In that window
//    GetFileAttributesExW, which no share mode blocks, already reports the new
//    write time, while an open without FILE_SHARE_DELETE - all _wfsopen can
//    express - fails with a sharing violation. A tight renamer/poller loop on
//    the development host hit it on 96 and 115 of 3000 renames; 0 of 6000 with
//    FILE_SHARE_DELETE.
//  - An in-place writer truncates, then writes. NTFS stamps both at clock-tick
//    resolution (15.6 ms on the development host), so the two usually share a
//    write time, and a poll that read the empty file in between had consumed
//    the write time the command arrived under.
//
// So the file is opened with FILE_SHARE_DELETE, and a failed open, a read
// error, or a read that found nothing at all leaves the write time unconsumed:
// the next 20 ms poll tries again. Other lockers (an exclusive opener, a
// scanner) cost a retry instead of a command. A file holding only blank lines
// is consumed without an acknowledgement, as it always was. A batch is read
// whole before any of it is applied, and only if the file still carries the
// write time it was noticed under, so a retry can never apply half a batch and
// a rewrite that lands mid-read cannot apply one twice.

enum class Retry { OpenFailed, ReadFailed, Empty, Changed };

// One write time the poller has not managed to read yet. Retries happen every
// poll. The log gets a line when an episode starts and, while opens or reads
// keep failing, more at doubling gaps. A write time finally read closes a
// printed episode with how long it took. A write time replaced after failures
// always gets a line, printed or not: that is the one way a batch still goes
// unapplied. Start and progress lines stay a second apart across episodes, so
// a writer that keeps rewriting an unreadable or empty file cannot flood the
// log; episodes that end before they could print are counted in the next line.
struct RetryEpisode {
    bool active = false;
    bool logged = false;      // printed its start line
    FILETIME stamp{};
    uint32_t failures = 0;    // failed opens and read errors
    uint32_t empties = 0;     // reads that found nothing
    uint32_t changes = 0;     // reads that found a later write than `stamp`
    DWORD lastError = 0;
    uint64_t firstMs = 0;
    uint64_t lastLineMs = 0;
};
RetryEpisode g_retry;
uint64_t g_retryLineMs = 0;   // last start or progress line from any episode
uint32_t g_unloggedEpisodes = 0;

const char* win32_error_text(DWORD code, char* out, size_t cap) {
    char text[160] = {};
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                             code, 0, text, sizeof(text), nullptr);
    while (n > 0 && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ' ||
                     text[n - 1] == '.'))
        text[--n] = '\0';
    if (n) sprintf_s(out, cap, "%lu, %s", code, text);
    else sprintf_s(out, cap, "%lu", code);
    return out;
}

// Close the current episode. `read` is the write time that was just read
// successfully, or null when a new write time replaced the one being retried.
void end_retry(const FILETIME* read) {
    if (!g_retry.active) return;
    char err[192] = "-";
    if (g_retry.failures) win32_error_text(g_retry.lastError, err, sizeof(err));
    const auto ms = static_cast<unsigned long long>(now_ms() - g_retry.firstMs);
    if (read && CompareFileTime(read, &g_retry.stamp) == 0) {
        if (g_retry.logged) {
            char what[384] = "";
            if (g_retry.failures)
                sprintf_s(what, "%u failed open/read(s) (last error %s)", g_retry.failures, err);
            if (g_retry.empties) {
                const size_t used = strlen(what);
                sprintf_s(what + used, sizeof(what) - used, "%s%u empty read(s)",
                          used ? " and " : "", g_retry.empties);
            }
            if (g_retry.changes) {
                const size_t used = strlen(what);
                sprintf_s(what + used, sizeof(what) - used, "%s%u read(s) of a later write",
                          used ? " and " : "", g_retry.changes);
            }
            XRSIM_LOG("xrsim: command.txt read after %s, %llu ms after the first attempt", what, ms);
        } else {
            ++g_unloggedEpisodes;
        }
    } else if (g_retry.failures) {
        // Printed whether or not the episode was: the version that kept failing
        // was replaced before it was ever applied. Not a loss when the writer
        // re-sent the same batch; a lost batch otherwise.
        XRSIM_LOG("xrsim: command.txt was rewritten before an earlier write could be read "
                  "(%u failed open/read(s) over %llu ms, last error %s)",
                  g_retry.failures, ms, err);
    } else if (!g_retry.logged) {
        ++g_unloggedEpisodes;
    }
    g_retry = RetryEpisode{};
}

void note_retry(const FILETIME& stamp, Retry why, DWORD error) {
    const uint64_t nowMs = now_ms();
    if (g_retry.active && CompareFileTime(&stamp, &g_retry.stamp) != 0) end_retry(nullptr);
    if (!g_retry.active) {
        g_retry.active = true;
        g_retry.stamp = stamp;
        g_retry.firstMs = nowMs;
    }
    switch (why) {
    case Retry::Empty: ++g_retry.empties; break;
    case Retry::Changed: ++g_retry.changes; break;
    default:
        ++g_retry.failures;
        g_retry.lastError = error;
        break;
    }

    if (g_retry.logged) {
        // Only failures repeat; an empty or rewritten file is told once.
        if (why == Retry::Empty || why == Retry::Changed) return;
        const uint64_t gap = g_retry.lastLineMs - g_retry.firstMs;
        if (nowMs - g_retry.lastLineMs < (gap > 1000 ? gap : 1000)) return;
    }
    if (g_retryLineMs && nowMs - g_retryLineMs < 1000) return;
    g_retry.logged = true;
    g_retry.lastLineMs = nowMs;
    g_retryLineMs = nowMs;
    char unlogged[64] = "";
    if (g_unloggedEpisodes) {
        sprintf_s(unlogged, "; %u shorter episode(s) not logged", g_unloggedEpisodes);
        g_unloggedEpisodes = 0;
    }

    const auto ms = static_cast<unsigned long long>(nowMs - g_retry.firstMs);
    if (why == Retry::Empty) {
        XRSIM_LOG("xrsim: command.txt is empty - write time left unconsumed (a writer may be "
                  "between truncate and write), re-reading every poll [empty read %u, %llu ms%s]",
                  g_retry.empties, ms, unlogged);
    } else if (why == Retry::Changed) {
        XRSIM_LOG("xrsim: command.txt was written again while being read - nothing applied under "
                  "the earlier write time, re-reading next poll [%llu ms%s]", ms, unlogged);
    } else {
        char err[192];
        XRSIM_LOG("xrsim: command.txt %s failed (error %s) - write time left unconsumed, retrying "
                  "every poll [failed attempt %u, %llu ms%s]",
                  why == Retry::OpenFailed ? "open" : "read",
                  win32_error_text(error, err, sizeof(err)), g_retry.failures, ms, unlogged);
    }
}

void poll_once() {
    wchar_t path[MAX_PATH];
    path_in_dir(path, MAX_PATH, L"command.txt");

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
    const FILETIME stamp = fad.ftLastWriteTime;
    if (CompareFileTime(&stamp, &g_lastWrite) == 0) return;

    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        note_retry(stamp, Retry::OpenFailed, GetLastError());
        return;
    }
    // Read through the CRT in text mode so lines split exactly as they did
    // under _wfsopen "rt" (CRLF folding, the 511-character fgets chunks).
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_TEXT);
    if (fd == -1) {
        CloseHandle(h);
        note_retry(stamp, Retry::OpenFailed, ERROR_TOO_MANY_OPEN_FILES);
        return;
    }
    FILE* f = _fdopen(fd, "rt");
    if (!f) {
        _close(fd);
        note_retry(stamp, Retry::OpenFailed, ERROR_TOO_MANY_OPEN_FILES);
        return;
    }

    std::vector<std::string> lines;
    bool readAnything = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        readAnything = true;
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '\0') continue;
        lines.emplace_back(line);
    }
    const bool readFailed = ferror(f) != 0;
    const DWORD readError = static_cast<DWORD>(_doserrno);
    // The batch must be the one `stamp` names. A write that landed between the
    // attribute query and the read - an in-place writer whose write fell a
    // clock tick after its truncation, or a rename-over - will be read again
    // under its own write time, so applying it under this one would apply it
    // twice.
    FILETIME openedStamp{};
    const bool changed = GetFileTime(h, nullptr, nullptr, &openedStamp) &&
                         CompareFileTime(&openedStamp, &stamp) != 0;
    fclose(f);
    if (readFailed) {
        note_retry(stamp, Retry::ReadFailed, readError);
        return;
    }
    if (changed) {
        note_retry(stamp, Retry::Changed, 0);
        return;
    }
    if (!readAnything) {
        note_retry(stamp, Retry::Empty, 0);
        return;
    }

    g_lastWrite = stamp;   // read, and not empty: only now is this write seen
    end_retry(&stamp);

    const uint32_t applied = static_cast<uint32_t>(lines.size());
    g_ackText[0] = '\0';
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g.lastCmdError[0] = '\0';
        for (const std::string& l : lines) {
            apply_line(l.c_str());
            if (strlen(g_ackText) + l.size() + 4 < sizeof(g_ackText)) {
                if (g_ackText[0]) strcat_s(g_ackText, " | ");
                strcat_s(g_ackText, l.c_str());
            }
        }
    }
    if (!applied) return;   // blank lines only: consumed, nothing to acknowledge

    g.cmdSeq.fetch_add(applied);
    g_ackFrame = static_cast<uint32_t>(snapshot().index);
    write_ack();
    // The control thread publishes state.json too, not just the frame path.
    // Under `pace step` with no credits xrWaitFrame is BLOCKED, so a
    // frame-path-only writer could never acknowledge the very `step` command
    // that unblocks it - the ack channel would deadlock by construction, the
    // same trap the dedicated poll thread exists to avoid.
    write_state_json();
    XRSIM_LOG("xrsim: applied %u command(s): %s", applied, g_ackText);
}

void thread_proc() {
    // A command.txt older than this process is last session's leftovers. Adopt
    // its timestamp so it is skipped rather than replayed.
    wchar_t path[MAX_PATH];
    path_in_dir(path, MAX_PATH, L"command.txt");
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        g_lastWrite = fad.ftLastWriteTime;
        XRSIM_LOG("xrsim: ignoring a command.txt written before this run started");
    }
    // A previous instance's retries are not this run's.
    g_retry = RetryEpisode{};
    g_retryLineMs = 0;
    g_unloggedEpisodes = 0;

    while (g_running.load()) {
        poll_once();
        Sleep(20); // 50 Hz - fast enough that `step` is never the bottleneck
    }
}

} // namespace

void rig_staging_init() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    rig_defaults(g_staging); // optics included - see the comment in rig_defaults
}

void control_start() {
    if (g_running.exchange(true)) return;
    g_startMs = now_ms();
    rig_staging_init();
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        g_dirty = true;
    }
    control_apply_pending();
    g_thread = std::thread(thread_proc);
    XRSIM_LOG("xrsim: control channel live at %ls", log::dir());
}

void control_stop() {
    if (!g_running.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
    // One final, unconditional write. Steady-state writes are rate-limited, so
    // without this the last file on disk lags the true final frame count by
    // however long the limiter was holding - and a script reading state.json
    // after the process exits gets a stale answer.
    write_state_json();
}

// THE COMMIT POINT. Called from inside xrWaitFrame.
void control_apply_pending() {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    const uint64_t nowMs = now_ms();
    const uint64_t frame = snapshot().index;

    // Expire timed holds on a frame boundary.
    for (uint32_t c = 0; c < VC_COUNT; ++c) {
        Hold& h = g_holds[c];
        if (!h.active) continue;
        const bool done = h.byFrames ? (frame >= h.untilFrame) : (nowMs >= h.untilMs);
        if (!done) continue;
        h.active = false;
        const auto vc = static_cast<VirtualControl>(c);
        set_control(g_staging, vc, false);
        if (vc == VC_TRIGGER_L) g_staging.trigger[0] = 0.0f;
        if (vc == VC_TRIGGER_R) g_staging.trigger[1] = 0.0f;
        if (vc == VC_SQUEEZE_L) g_staging.squeeze[0] = 0.0f;
        if (vc == VC_SQUEEZE_R) g_staging.squeeze[1] = 0.0f;
        g_dirty = true;
    }

    if (g_headMotion.active) {
        const uint64_t el = nowMs - g_headMotion.startMs;
        float t = g_headMotion.durMs ? static_cast<float>(el) / g_headMotion.durMs : 1.0f;
        if (t >= 1.0f) { t = 1.0f; g_headMotion.active = false; }
        g_staging.head = pose_lerp(g_headMotion.from, g_headMotion.to,
                                   g_headMotion.linear ? t : ease_smooth(t));
        g_dirty = true;
    }

    for (int h = 0; h < 2; ++h) {
        Motion& m = g_handMotion[h];
        if (!m.active) continue;
        const uint64_t el = nowMs - m.startMs;
        float t = m.durMs ? static_cast<float>(el) / m.durMs : 1.0f;
        if (t >= 1.0f) { t = 1.0f; m.active = false; }
        const Pose p = pose_lerp(m.from, m.to, m.linear ? t : ease_smooth(t));
        if (m.isAim) g_staging.aim[h] = p; else g_staging.grip[h] = p;
        g_dirty = true;
    }

    if (g_orbit.active) {
        const uint64_t el = nowMs - g_orbit.startMs;
        if (el >= g_orbit.durMs) g_orbit.active = false;
        const float yaw = g_orbit.baseYaw + g_orbit.degPerSec * (static_cast<float>(el) / 1000.0f);
        float y, p, r;
        quat_to_ypr(g_staging.head.q, y, p, r);
        g_staging.head.q = quat_from_ypr(deg2rad(yaw), p, r);
        g_dirty = true;
    }

    if (!g_dirty) return;
    g_dirty = false;
    // The frame path owns the snapshot rig; this is the single write.
    snapshot_set_rig(g_staging);
}

void control_write_state() {
    const uint32_t hz = g.stateWriteHz.load();
    const uint64_t nowMs = now_ms();
    // Every frame under `pace step` (determinism needs it), rate-limited when
    // free-running, because 90 file rewrites a second is real I/O for nothing.
    if (g.pacing.mode != PaceMode::Step && hz > 0) {
        const uint64_t minGap = 1000 / (hz ? hz : 1);
        if (nowMs - g_lastStateWriteMs < minGap) return;
    }
    g_lastStateWriteMs = nowMs;
    write_state_json();
}

} // namespace xrsim
