// xr-sim: action sets, actions, interaction profiles, and action state.
//
// The binding table below is the sim's contract with core/vr/openxr_input.cpp.
// It carries the whole oculus/touch_controller component list, not only the 19
// paths the mod binds today, so that a binding the mod ADDS later is validated
// rather than silently ignored - an unknown path returns PATH_UNSUPPORTED and
// logs the exact string, which is the sim catching a typo a real runtime would
// swallow.

#include "xrsim_internal.h"

#include <cmath>
#include <cstring>

namespace xrsim {
namespace {

std::mutex g_mutex;
SimActionSet g_sets[kMaxActionSets];
SimAction g_actions[kMaxActions];
bool g_attached = false;
XrPath g_profileTouch = XR_NULL_PATH;
XrPath g_profileSimple = XR_NULL_PATH;
bool g_useSimpleProfile = false;
uint64_t g_lastSyncFrame = 0;
// Bumped per xrSuggestInteractionProfileBindings call, so an action can tell
// its first binding in a call from a later one.
uint32_t g_suggestGen = 0;

struct BindingEntry {
    const char* path;
    VirtualControl control;  // VC_NONE = a real profile path we accept but do not drive
    int hand;
};

// The full oculus/touch_controller component set. Order does not matter; this is
// a lookup table, not a priority list.
const BindingEntry kTouch[] = {
    {"/user/hand/left/input/thumbstick", VC_STICK_L, 0},
    {"/user/hand/right/input/thumbstick", VC_STICK_R, 1},
    {"/user/hand/left/input/trigger/value", VC_TRIGGER_L, 0},
    {"/user/hand/right/input/trigger/value", VC_TRIGGER_R, 1},
    {"/user/hand/left/input/squeeze/value", VC_SQUEEZE_L, 0},
    {"/user/hand/right/input/squeeze/value", VC_SQUEEZE_R, 1},
    {"/user/hand/right/input/a/click", VC_BTN_A, 1},
    {"/user/hand/right/input/b/click", VC_BTN_B, 1},
    {"/user/hand/left/input/x/click", VC_BTN_X, 0},
    {"/user/hand/left/input/y/click", VC_BTN_Y, 0},
    {"/user/hand/left/input/menu/click", VC_MENU, 0},
    {"/user/hand/left/input/thumbstick/click", VC_CLICK_L, 0},
    {"/user/hand/right/input/thumbstick/click", VC_CLICK_R, 1},
    {"/user/hand/left/input/thumbrest/touch", VC_REST_L, 0},
    {"/user/hand/right/input/thumbrest/touch", VC_REST_R, 1},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    // Accepted, not driven. Present so the profile is honest about what a real
    // Touch controller exposes.
    {"/user/hand/left/input/thumbstick/x", VC_NONE, 0},
    {"/user/hand/left/input/thumbstick/y", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/x", VC_NONE, 1},
    {"/user/hand/right/input/thumbstick/y", VC_NONE, 1},
    {"/user/hand/left/input/thumbstick/touch", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/touch", VC_NONE, 1},
    {"/user/hand/left/input/trigger/touch", VC_NONE, 0},
    {"/user/hand/right/input/trigger/touch", VC_NONE, 1},
    {"/user/hand/right/input/a/touch", VC_NONE, 1},
    {"/user/hand/right/input/b/touch", VC_NONE, 1},
    {"/user/hand/left/input/x/touch", VC_NONE, 0},
    {"/user/hand/left/input/y/touch", VC_NONE, 0},
    {"/user/hand/left/input/system/click", VC_NONE, 0},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

// khr/simple_controller: the fallback the mod suggests best-effort, so the
// "unknown runtime" path stays exercisable.
const BindingEntry kSimple[] = {
    {"/user/hand/left/input/select/click", VC_BTN_X, 0},
    {"/user/hand/right/input/select/click", VC_BTN_A, 1},
    {"/user/hand/left/input/menu/click", VC_MENU, 0},
    {"/user/hand/right/input/menu/click", VC_MENU, 1},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

// valve/index_controller: full component set (s62). Driven controls mirror the
// mod's native Index table: A/B exist on BOTH hands (left a/b feed X/Y), menu
// is a firm left-trackpad press (boolean action on the float force component),
// no thumbrest exists so VC_REST never resolves on this profile.
const BindingEntry kIndex[] = {
    {"/user/hand/left/input/thumbstick", VC_STICK_L, 0},
    {"/user/hand/right/input/thumbstick", VC_STICK_R, 1},
    {"/user/hand/left/input/trigger/value", VC_TRIGGER_L, 0},
    {"/user/hand/right/input/trigger/value", VC_TRIGGER_R, 1},
    {"/user/hand/left/input/squeeze/value", VC_SQUEEZE_L, 0},
    {"/user/hand/right/input/squeeze/value", VC_SQUEEZE_R, 1},
    {"/user/hand/right/input/a/click", VC_BTN_A, 1},
    {"/user/hand/right/input/b/click", VC_BTN_B, 1},
    {"/user/hand/left/input/a/click", VC_BTN_X, 0},
    {"/user/hand/left/input/b/click", VC_BTN_Y, 0},
    {"/user/hand/left/input/thumbstick/click", VC_CLICK_L, 0},
    {"/user/hand/right/input/thumbstick/click", VC_CLICK_R, 1},
    {"/user/hand/left/input/trackpad/force", VC_MENU, 0},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    // Accepted, not driven - what a real Index controller exposes.
    {"/user/hand/left/input/system/click", VC_NONE, 0},
    {"/user/hand/right/input/system/click", VC_NONE, 1},
    {"/user/hand/left/input/system/touch", VC_NONE, 0},
    {"/user/hand/right/input/system/touch", VC_NONE, 1},
    {"/user/hand/left/input/a/touch", VC_NONE, 0},
    {"/user/hand/right/input/a/touch", VC_NONE, 1},
    {"/user/hand/left/input/b/touch", VC_NONE, 0},
    {"/user/hand/right/input/b/touch", VC_NONE, 1},
    {"/user/hand/left/input/squeeze/force", VC_NONE, 0},
    {"/user/hand/right/input/squeeze/force", VC_NONE, 1},
    {"/user/hand/left/input/trigger/click", VC_NONE, 0},
    {"/user/hand/right/input/trigger/click", VC_NONE, 1},
    {"/user/hand/left/input/trigger/touch", VC_NONE, 0},
    {"/user/hand/right/input/trigger/touch", VC_NONE, 1},
    {"/user/hand/left/input/thumbstick/x", VC_NONE, 0},
    {"/user/hand/left/input/thumbstick/y", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/x", VC_NONE, 1},
    {"/user/hand/right/input/thumbstick/y", VC_NONE, 1},
    {"/user/hand/left/input/thumbstick/touch", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/touch", VC_NONE, 1},
    {"/user/hand/left/input/trackpad", VC_NONE, 0},
    {"/user/hand/right/input/trackpad", VC_NONE, 1},
    {"/user/hand/left/input/trackpad/x", VC_NONE, 0},
    {"/user/hand/left/input/trackpad/y", VC_NONE, 0},
    {"/user/hand/right/input/trackpad/x", VC_NONE, 1},
    {"/user/hand/right/input/trackpad/y", VC_NONE, 1},
    {"/user/hand/right/input/trackpad/force", VC_NONE, 1},
    {"/user/hand/left/input/trackpad/touch", VC_NONE, 0},
    {"/user/hand/right/input/trackpad/touch", VC_NONE, 1},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

// htc/vive_controller: wands. Trackpad parent path feeds the sticks; squeeze
// is a CLICK (boolean) that the mod binds to its FLOAT grip actions - the sim
// maps it onto the analog squeeze rig channel, mirroring the runtime's 0/1
// conversion closely enough for the flat fence. Right menu/click feeds BTN_A
// (the mod's use/interact) - table-local mapping, deliberately different from
// kSimple's right menu.
const BindingEntry kVive[] = {
    {"/user/hand/left/input/trackpad", VC_STICK_L, 0},
    {"/user/hand/right/input/trackpad", VC_STICK_R, 1},
    {"/user/hand/left/input/trigger/value", VC_TRIGGER_L, 0},
    {"/user/hand/right/input/trigger/value", VC_TRIGGER_R, 1},
    {"/user/hand/left/input/squeeze/click", VC_SQUEEZE_L, 0},
    {"/user/hand/right/input/squeeze/click", VC_SQUEEZE_R, 1},
    {"/user/hand/right/input/menu/click", VC_BTN_A, 1},
    {"/user/hand/left/input/menu/click", VC_MENU, 0},
    {"/user/hand/left/input/trackpad/click", VC_CLICK_L, 0},
    {"/user/hand/right/input/trackpad/click", VC_CLICK_R, 1},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    // Accepted, not driven.
    {"/user/hand/left/input/system/click", VC_NONE, 0},
    {"/user/hand/right/input/system/click", VC_NONE, 1},
    {"/user/hand/left/input/trigger/click", VC_NONE, 0},
    {"/user/hand/right/input/trigger/click", VC_NONE, 1},
    {"/user/hand/left/input/trackpad/x", VC_NONE, 0},
    {"/user/hand/left/input/trackpad/y", VC_NONE, 0},
    {"/user/hand/right/input/trackpad/x", VC_NONE, 1},
    {"/user/hand/right/input/trackpad/y", VC_NONE, 1},
    {"/user/hand/left/input/trackpad/touch", VC_NONE, 0},
    {"/user/hand/right/input/trackpad/touch", VC_NONE, 1},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

// microsoft/motion_controller: thumbstick + trackpad, digital squeeze (same
// analog-channel mapping argument as Vive), no face buttons - trackpad clicks
// feed BTN_A (right) and BTN_X (left).
const BindingEntry kWmr[] = {
    {"/user/hand/left/input/thumbstick", VC_STICK_L, 0},
    {"/user/hand/right/input/thumbstick", VC_STICK_R, 1},
    {"/user/hand/left/input/trigger/value", VC_TRIGGER_L, 0},
    {"/user/hand/right/input/trigger/value", VC_TRIGGER_R, 1},
    {"/user/hand/left/input/squeeze/click", VC_SQUEEZE_L, 0},
    {"/user/hand/right/input/squeeze/click", VC_SQUEEZE_R, 1},
    {"/user/hand/right/input/trackpad/click", VC_BTN_A, 1},
    {"/user/hand/left/input/trackpad/click", VC_BTN_X, 0},
    {"/user/hand/left/input/thumbstick/click", VC_CLICK_L, 0},
    {"/user/hand/right/input/thumbstick/click", VC_CLICK_R, 1},
    {"/user/hand/left/input/menu/click", VC_MENU, 0},
    {"/user/hand/left/input/grip/pose", VC_POSE_GRIP_L, 0},
    {"/user/hand/right/input/grip/pose", VC_POSE_GRIP_R, 1},
    {"/user/hand/left/input/aim/pose", VC_POSE_AIM_L, 0},
    {"/user/hand/right/input/aim/pose", VC_POSE_AIM_R, 1},
    // Accepted, not driven.
    {"/user/hand/right/input/menu/click", VC_NONE, 1},
    {"/user/hand/left/input/thumbstick/x", VC_NONE, 0},
    {"/user/hand/left/input/thumbstick/y", VC_NONE, 0},
    {"/user/hand/right/input/thumbstick/x", VC_NONE, 1},
    {"/user/hand/right/input/thumbstick/y", VC_NONE, 1},
    {"/user/hand/left/input/trackpad", VC_NONE, 0},
    {"/user/hand/right/input/trackpad", VC_NONE, 1},
    {"/user/hand/left/input/trackpad/x", VC_NONE, 0},
    {"/user/hand/left/input/trackpad/y", VC_NONE, 0},
    {"/user/hand/right/input/trackpad/x", VC_NONE, 1},
    {"/user/hand/right/input/trackpad/y", VC_NONE, 1},
    {"/user/hand/left/input/trackpad/touch", VC_NONE, 0},
    {"/user/hand/right/input/trackpad/touch", VC_NONE, 1},
    {"/user/hand/left/output/haptic", VC_NONE, 0},
    {"/user/hand/right/output/haptic", VC_NONE, 1},
};

// Profile registry. `authoritative` marks the ONE profile whose bindings may
// overwrite an already-resolved control - the sim presents as a Quest 3, so
// touch stays the profile the rig actually drives; every other table only
// fills VC_NONE slots (first-writer), preserving the s62 typo-catcher without
// letting a SteamVR-family suggestion steal a resolved touch control.
struct ProfileEntry {
    const char* profile;
    const char* shortName;
    const BindingEntry* table;
    size_t n;
    bool authoritative;
};
const ProfileEntry kProfiles[] = {
    {"/interaction_profiles/oculus/touch_controller", "touch", kTouch,
     sizeof(kTouch) / sizeof(kTouch[0]), true},
    {"/interaction_profiles/khr/simple_controller", "simple", kSimple,
     sizeof(kSimple) / sizeof(kSimple[0]), false},
    {"/interaction_profiles/valve/index_controller", "index", kIndex,
     sizeof(kIndex) / sizeof(kIndex[0]), false},
    {"/interaction_profiles/htc/vive_controller", "vive", kVive,
     sizeof(kVive) / sizeof(kVive[0]), false},
    {"/interaction_profiles/microsoft/motion_controller", "wmr", kWmr,
     sizeof(kWmr) / sizeof(kWmr[0]), false},
};

const BindingEntry* find_binding(const BindingEntry* table, size_t n, const char* path) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(table[i].path, path) == 0) return &table[i];
    return nullptr;
}

bool control_is_pose(VirtualControl c) {
    return c == VC_POSE_GRIP_L || c == VC_POSE_GRIP_R || c == VC_POSE_AIM_L || c == VC_POSE_AIM_R;
}

// Read a control out of the committed rig. One place, so xrGetActionState* is a
// table read and cannot disagree with what the capture JSON reports.
float control_float(const Rig& rig, VirtualControl c) {
    switch (c) {
    case VC_TRIGGER_L: return rig.trigger[0];
    case VC_TRIGGER_R: return rig.trigger[1];
    case VC_SQUEEZE_L: return rig.squeeze[0];
    case VC_SQUEEZE_R: return rig.squeeze[1];
    default: return 0.0f;
    }
}

bool control_bool(const Rig& rig, VirtualControl c) {
    switch (c) {
    case VC_BTN_A: return rig.btnA;
    case VC_BTN_B: return rig.btnB;
    case VC_BTN_X: return rig.btnX;
    case VC_BTN_Y: return rig.btnY;
    case VC_MENU: return rig.menu;
    case VC_CLICK_L: return rig.click[0];
    case VC_CLICK_R: return rig.click[1];
    case VC_REST_L: return rig.rest[0];
    case VC_REST_R: return rig.rest[1];
    default: return false;
    }
}

} // namespace

SimAction* action_get(XrAction h) {
    if (h == XR_NULL_HANDLE || handle_type(h) != HT_ACTION) return nullptr;
    const uint32_t i = handle_index(h);
    if (i >= kMaxActions) return nullptr;
    SimAction& a = g_actions[i];
    if (!a.used || a.gen != handle_gen(h)) return nullptr;
    return &a;
}

SimAction* action_get_by_index(uint32_t index) {
    if (index >= kMaxActions) return nullptr;
    SimAction& a = g_actions[index];
    return a.used ? &a : nullptr;
}

bool actions_attached() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_attached;
}

void actions_reset_session() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Action sets survive a session (they belong to the instance), so only the
    // attachment is cleared. The mod re-attaches the SAME set to a new session
    // after a teardown and re-bring-up, and that must be accepted.
    g_attached = false;
    for (auto& s : g_sets)
        if (s.used) s.attached = false;
    for (auto& a : g_actions) {
        if (!a.used) continue;
        for (SimActionLane& lane : a.lane) lane = SimActionLane{};
    }
}

void actions_set_profile(bool simple) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_useSimpleProfile = simple;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

static XrResult impl_CreateActionSet(XrInstance instance, const XrActionSetCreateInfo* info,
                                     XrActionSet* out) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < kMaxActionSets; ++i) {
        if (g_sets[i].used) continue;
        const uint32_t gen = g_sets[i].gen + 1;
        g_sets[i] = SimActionSet{};
        g_sets[i].used = true;
        g_sets[i].gen = gen;
        strcpy_s(g_sets[i].name, info->actionSetName);
        *out = make_typed_handle<XrActionSet>(HT_ACTIONSET, i, gen);
        XRSIM_LOG("xrsim: action set '%s' created", info->actionSetName);
        return XR_SUCCESS;
    }
    return XR_ERROR_LIMIT_REACHED;
}

static XrResult impl_DestroyActionSet(XrActionSet handle) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (handle == XR_NULL_HANDLE || handle_type(handle) != HT_ACTIONSET)
        return XR_ERROR_HANDLE_INVALID;
    const uint32_t i = handle_index(handle);
    if (i >= kMaxActionSets || !g_sets[i].used || g_sets[i].gen != handle_gen(handle))
        return XR_ERROR_HANDLE_INVALID;
    g_sets[i].used = false;
    for (auto& a : g_actions)
        if (a.used && a.setIndex == i) a.used = false;
    return XR_SUCCESS;
}

static XrResult impl_CreateAction(XrActionSet setHandle, const XrActionCreateInfo* info,
                                  XrAction* out) noexcept {
    if (!info || !out) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (setHandle == XR_NULL_HANDLE || handle_type(setHandle) != HT_ACTIONSET)
        return XR_ERROR_HANDLE_INVALID;
    const uint32_t si = handle_index(setHandle);
    if (si >= kMaxActionSets || !g_sets[si].used || g_sets[si].gen != handle_gen(setHandle))
        return XR_ERROR_HANDLE_INVALID;
    if (g_sets[si].attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;

    for (uint32_t i = 0; i < kMaxActions; ++i) {
        if (g_actions[i].used) continue;
        const uint32_t gen = g_actions[i].gen + 1;
        g_actions[i] = SimAction{};
        g_actions[i].used = true;
        g_actions[i].gen = gen;
        g_actions[i].setIndex = si;
        g_actions[i].type = info->actionType;
        strcpy_s(g_actions[i].name, info->actionName);
        if (info->subactionPaths) {
            uint32_t n = info->countSubactionPaths;
            if (n > kMaxSubactionPaths) {
                XRSIM_LOG("xrsim: action '%s' declares %u subaction paths; the first %u are kept",
                          info->actionName, n, kMaxSubactionPaths);
                n = kMaxSubactionPaths;
            }
            for (uint32_t k = 0; k < n; ++k) g_actions[i].subactionPaths[k] = info->subactionPaths[k];
            g_actions[i].subactionCount = n;
        }
        *out = make_typed_handle<XrAction>(HT_ACTION, i, gen);
        return XR_SUCCESS;
    }
    return XR_ERROR_LIMIT_REACHED;
}

static XrResult impl_DestroyAction(XrAction handle) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    SimAction* a = action_get(handle);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    a->used = false;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

static XrResult impl_SuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* info) noexcept {
    if (!valid_instance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;

    const char* profile = path_str(info->interactionProfile);
    const ProfileEntry* pe = nullptr;
    for (const ProfileEntry& cand : kProfiles) {
        if (profile && strcmp(profile, cand.profile) == 0) {
            pe = &cand;
            break;
        }
    }
    if (!pe) {
        XRSIM_LOG("xrsim: interaction profile '%s' is not supported by this runtime",
                  profile ? profile : "(null)");
        return XR_ERROR_PATH_UNSUPPORTED;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;

    uint32_t bound = 0;
    ++g_suggestGen;
    for (uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = info->suggestedBindings[i];
        const char* p = path_str(b.binding);
        const BindingEntry* e = find_binding(pe->table, pe->n, p ? p : "");
        if (!e) {
            XRSIM_LOG("xrsim: binding path '%s' is not on the %s profile - REJECTED",
                      p ? p : "(null)", pe->shortName);
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        SimAction* a = action_get(b.action);
        if (!a) return XR_ERROR_HANDLE_INVALID;

        // The touch profile is the one the mod actually runs on; no other
        // profile's suggestion may overwrite a resolved control.
        if (pe->authoritative || a->control == VC_NONE) {
            a->control = e->control;
            a->hand = e->hand;
        }

        // Value reads keep EVERY driven binding of the winning profile, not just
        // the last. Touch replaces whatever another profile left; any other
        // profile only claims an action no profile has driven yet.
        const bool driven = e->control != VC_NONE;
        bool append = false;
        if (pe->authoritative) {
            if (a->suggestGen != g_suggestGen) {
                a->bindingCount = 0;
                a->bindingsAuthoritative = true;
                a->suggestGen = g_suggestGen;
            }
            append = driven;
        } else if (driven && !a->bindingsAuthoritative
                   && (a->bindingCount == 0 || a->suggestGen == g_suggestGen)) {
            a->suggestGen = g_suggestGen;
            append = true;
        }
        for (uint32_t k = 0; append && k < a->bindingCount; ++k)
            if (a->bindings[k].control == e->control && a->bindings[k].hand == e->hand)
                append = false;   // the same binding suggested twice
        if (append) {
            if (a->bindingCount < kMaxActionBindings) {
                a->bindings[a->bindingCount++] = SimBinding{e->control, e->hand};
            } else {
                XRSIM_LOG("xrsim: action '%s' exceeds %u bindings; '%s' is ignored", a->name,
                          kMaxActionBindings, p ? p : "(null)");
            }
        }
        if (e->control != VC_NONE) ++bound;
    }

    XRSIM_LOG("xrsim: %u binding(s) suggested on the %s profile, %u driven",
              info->countSuggestedBindings, pe->shortName, bound);
    return XR_SUCCESS;
}

static XrResult impl_AttachSessionActionSets(XrSession session,
                                             const XrSessionActionSetsAttachInfo* info) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (g.hazards.attachFail) return XR_ERROR_RUNTIME_FAILURE;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < info->countActionSets; ++i) {
        const XrActionSet h = info->actionSets[i];
        if (h == XR_NULL_HANDLE || handle_type(h) != HT_ACTIONSET) return XR_ERROR_HANDLE_INVALID;
        const uint32_t si = handle_index(h);
        if (si >= kMaxActionSets || !g_sets[si].used || g_sets[si].gen != handle_gen(h))
            return XR_ERROR_HANDLE_INVALID;
        g_sets[si].attached = true;
    }
    g_attached = true;

    // Emit the profile-changed event a real runtime sends once bindings resolve.
    XrEventDataBuffer buf{};
    auto* ev = reinterpret_cast<XrEventDataInteractionProfileChanged*>(&buf);
    ev->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    ev->session = session;
    queue_event(buf);

    XRSIM_LOG("xrsim: action set(s) attached to the session");
    return XR_SUCCESS;
}

static XrResult impl_GetCurrentInteractionProfile(XrSession session, XrPath,
                                                  XrInteractionProfileState* state) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (g_profileTouch == XR_NULL_PATH)
        g_profileTouch = path_intern("/interaction_profiles/oculus/touch_controller");
    if (g_profileSimple == XR_NULL_PATH)
        g_profileSimple = path_intern("/interaction_profiles/khr/simple_controller");
    state->interactionProfile = g_useSimpleProfile ? g_profileSimple : g_profileTouch;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Sync and state
// ---------------------------------------------------------------------------

static XrResult impl_SyncActions(XrSession session, const XrActionsSyncInfo* info) noexcept {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (info->countActiveActionSets != 0 && !info->activeActionSets)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!actions_attached()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    if (!session_is_running()) return XR_ERROR_SESSION_NOT_RUNNING;

    const FrameSnapshot snap = snapshot();
    g_lastSyncFrame = snap.index;

    // XR_SESSION_NOT_FOCUSED is a SUCCESS-class code (8). The mod checks for it
    // BEFORE its XR_FAILED test and treats it as success, publishing a zeroed
    // pad and invalidating its hand slots - that behaviour is exactly what the
    // `focus lose` hazard is meant to exercise, so returning it faithfully is
    // the whole point.
    if (current_session_state() != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;

    bool activeSets[kMaxActionSets]{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (uint32_t i = 0; i < info->countActiveActionSets; ++i) {
            const XrActionSet handle = info->activeActionSets[i].actionSet;
            if (handle == XR_NULL_HANDLE || handle_type(handle) != HT_ACTIONSET)
                return XR_ERROR_HANDLE_INVALID;
            const uint32_t setIndex = handle_index(handle);
            if (setIndex >= kMaxActionSets || !g_sets[setIndex].used
                || g_sets[setIndex].gen != handle_gen(handle))
                return XR_ERROR_HANDLE_INVALID;
            if (!g_sets[setIndex].attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
            activeSets[setIndex] = true;
        }

        const Rig& rig = snap.rig;
        for (SimAction& action : g_actions) {
            if (!action.used) continue;
            const bool setActive = activeSets[action.setIndex];

            for (int laneIndex = 0; laneIndex < kLaneCount; ++laneIndex) {
                SimActionLane& lane = action.lane[laneIndex];

                // Aggregate the bindings that feed this lane, by the OpenXR rule for
                // an action bound to several sources: booleans OR, floats take the
                // largest magnitude, vectors the longest.
                bool active = false;
                bool boolValue = false;
                float floatValue = 0.0f;
                XrVector2f vectorValue{};
                float vectorLength2 = -1.0f;
                for (uint32_t b = 0; b < action.bindingCount; ++b) {
                    const SimBinding& bind = action.bindings[b];
                    if (laneIndex != kLaneAny && bind.hand != laneIndex) continue;
                    const bool bindActive = setActive && bind.control != VC_NONE
                        && (!control_is_pose(bind.control) || rig.handValid[bind.hand == 1 ? 1 : 0]);
                    if (!bindActive) continue;
                    active = true;
                    if (action.type == XR_ACTION_TYPE_BOOLEAN_INPUT) {
                        boolValue = boolValue || control_bool(rig, bind.control);
                    } else if (action.type == XR_ACTION_TYPE_FLOAT_INPUT) {
                        const float v = control_float(rig, bind.control);
                        if (std::fabs(v) > std::fabs(floatValue)) floatValue = v;
                    } else if (action.type == XR_ACTION_TYPE_VECTOR2F_INPUT
                               && (bind.control == VC_STICK_L || bind.control == VC_STICK_R)) {
                        const int stick = bind.control == VC_STICK_R ? 1 : 0;
                        const XrVector2f v{rig.stick[stick][0], rig.stick[stick][1]};
                        const float length2 = v.x * v.x + v.y * v.y;
                        if (length2 > vectorLength2) {
                            vectorLength2 = length2;
                            vectorValue = v;
                        }
                    }
                }

                // From here on, exactly the single-binding edge logic, per lane.
                const bool wasInitialized = lane.syncInitialized;
                const bool wasActive = lane.syncedActive;
                bool valueChanged = false;
                if (action.type == XR_ACTION_TYPE_BOOLEAN_INPUT) {
                    const bool value = active && boolValue;
                    valueChanged = wasInitialized && wasActive && active && value != lane.syncedBool;
                    lane.syncedBool = value;
                } else if (action.type == XR_ACTION_TYPE_FLOAT_INPUT) {
                    const float value = active ? floatValue : 0.0f;
                    valueChanged = wasInitialized && wasActive && active && value != lane.syncedFloat;
                    lane.syncedFloat = value;
                } else if (action.type == XR_ACTION_TYPE_VECTOR2F_INPUT) {
                    const XrVector2f value = active ? vectorValue : XrVector2f{};
                    valueChanged = wasInitialized && wasActive && active
                        && (value.x != lane.syncedVector.x || value.y != lane.syncedVector.y);
                    lane.syncedVector = value;
                }

                lane.changedSinceLastSync = valueChanged;
                lane.syncedActive = active;
                lane.syncInitialized = true;
                if (valueChanged) lane.lastChangeTime = snap.displayTime;
            }
        }
    }
    return XR_SUCCESS;
}

namespace {

// Common preamble for the four state getters. It also maps the subaction path to
// a lane: NULL reads the combined state, a declared hand reads that hand, and a
// path the action never declared is an error on every conformant runtime.
XrResult resolve(XrSession session, const XrActionStateGetInfo* info, SimAction** out, int* lane) {
    if (!session_valid(session)) return XR_ERROR_HANDLE_INVALID;
    if (!info) return XR_ERROR_VALIDATION_FAILURE;
    if (!actions_attached()) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    SimAction* a = action_get(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    *lane = kLaneAny;
    if (info->subactionPath != XR_NULL_PATH) {
        bool declared = false;
        for (uint32_t i = 0; i < a->subactionCount && !declared; ++i)
            declared = a->subactionPaths[i] == info->subactionPath;
        const char* p = path_str(info->subactionPath);
        if (!declared) {
            if (!a->warnedUndeclaredSubaction) {
                a->warnedUndeclaredSubaction = true;
                XRSIM_LOG("xrsim: action '%s' read with subaction path '%s' it never declared - REJECTED",
                          a->name, p ? p : "(null)");
            }
            return XR_ERROR_PATH_UNSUPPORTED;
        }
        if (p && strcmp(p, "/user/hand/left") == 0) *lane = kLaneLeft;
        else if (p && strcmp(p, "/user/hand/right") == 0) *lane = kLaneRight;
        else *lane = -1;   // declared but not modelled, e.g. /user/gamepad: no input
    }
    *out = a;
    return XR_SUCCESS;
}

// A pose action reports active only through a pose binding on the lane it reads.
bool lane_has_pose(const SimAction& a, int lane) {
    for (uint32_t b = 0; b < a.bindingCount; ++b)
        if ((lane == kLaneAny || a.bindings[b].hand == lane) && control_is_pose(a.bindings[b].control))
            return true;
    return false;
}

bool inputs_live() { return current_session_state() == XR_SESSION_STATE_FOCUSED; }

} // namespace

static XrResult impl_GetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info,
                                           XrActionStateBoolean* state) noexcept {
    SimAction* a = nullptr;
    int lane = kLaneAny;
    const XrResult r = resolve(session, info, &a, &lane);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_BOOLEAN_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const SimActionLane* L = lane >= 0 ? &a->lane[lane] : nullptr;
    const bool live = inputs_live() && L && L->syncedActive;
    state->type = XR_TYPE_ACTION_STATE_BOOLEAN;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    state->currentState = live && L->syncedBool ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = live && L->changedSinceLastSync ? XR_TRUE : XR_FALSE;
    state->lastChangeTime = live ? L->lastChangeTime : 0;
    return XR_SUCCESS;
}

static XrResult impl_GetActionStateFloat(XrSession session, const XrActionStateGetInfo* info,
                                         XrActionStateFloat* state) noexcept {
    SimAction* a = nullptr;
    int lane = kLaneAny;
    const XrResult r = resolve(session, info, &a, &lane);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_FLOAT_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const SimActionLane* L = lane >= 0 ? &a->lane[lane] : nullptr;
    const bool live = inputs_live() && L && L->syncedActive;
    state->type = XR_TYPE_ACTION_STATE_FLOAT;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    state->currentState = live ? L->syncedFloat : 0.0f;
    state->changedSinceLastSync = live && L->changedSinceLastSync ? XR_TRUE : XR_FALSE;
    state->lastChangeTime = live ? L->lastChangeTime : 0;
    return XR_SUCCESS;
}

static XrResult impl_GetActionStateVector2f(XrSession session, const XrActionStateGetInfo* info,
                                            XrActionStateVector2f* state) noexcept {
    SimAction* a = nullptr;
    int lane = kLaneAny;
    const XrResult r = resolve(session, info, &a, &lane);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_VECTOR2F_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const SimActionLane* L = lane >= 0 ? &a->lane[lane] : nullptr;
    const bool live = inputs_live() && L && L->syncedActive;
    state->type = XR_TYPE_ACTION_STATE_VECTOR2F;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    // Raw, pre-deadzone. The mod applies its own radial deadzone and would
    // double-apply if the runtime helped.
    state->currentState = live ? L->syncedVector : XrVector2f{};
    state->changedSinceLastSync = live && L->changedSinceLastSync ? XR_TRUE : XR_FALSE;
    state->lastChangeTime = live ? L->lastChangeTime : 0;
    return XR_SUCCESS;
}

static XrResult impl_GetActionStatePose(XrSession session, const XrActionStateGetInfo* info,
                                        XrActionStatePose* state) noexcept {
    SimAction* a = nullptr;
    int lane = kLaneAny;
    const XrResult r = resolve(session, info, &a, &lane);
    if (XR_FAILED(r)) return r;
    if (!state) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;

    const SimActionLane* L = lane >= 0 ? &a->lane[lane] : nullptr;
    const bool live = inputs_live() && L && L->syncedActive && lane_has_pose(*a, lane);
    state->type = XR_TYPE_ACTION_STATE_POSE;
    state->isActive = live ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Shims
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateActionSet(XrInstance i, const XrActionSetCreateInfo* ci,
                                                     XrActionSet* o) {
    XRSIM_ENTRY(impl_CreateActionSet(i, ci, o), "CreateActionSet")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyActionSet(XrActionSet s) {
    XRSIM_ENTRY(impl_DestroyActionSet(s), "DestroyActionSet")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_CreateAction(XrActionSet s, const XrActionCreateInfo* ci,
                                                  XrAction* o) {
    XRSIM_ENTRY(impl_CreateAction(s, ci, o), "CreateAction")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_DestroyAction(XrAction a) {
    XRSIM_ENTRY(impl_DestroyAction(a), "DestroyAction")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SuggestInteractionProfileBindings(
    XrInstance i, const XrInteractionProfileSuggestedBinding* b) {
    XRSIM_ENTRY(impl_SuggestInteractionProfileBindings(i, b), "SuggestInteractionProfileBindings")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_AttachSessionActionSets(
    XrSession s, const XrSessionActionSetsAttachInfo* i) {
    XRSIM_ENTRY(impl_AttachSessionActionSets(s, i), "AttachSessionActionSets")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetCurrentInteractionProfile(XrSession s, XrPath p,
                                                                  XrInteractionProfileState* st) {
    XRSIM_ENTRY(impl_GetCurrentInteractionProfile(s, p, st), "GetCurrentInteractionProfile")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateBoolean(XrSession s,
                                                           const XrActionStateGetInfo* i,
                                                           XrActionStateBoolean* st) {
    XRSIM_ENTRY(impl_GetActionStateBoolean(s, i, st), "GetActionStateBoolean")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateFloat(XrSession s, const XrActionStateGetInfo* i,
                                                         XrActionStateFloat* st) {
    XRSIM_ENTRY(impl_GetActionStateFloat(s, i, st), "GetActionStateFloat")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStateVector2f(XrSession s,
                                                            const XrActionStateGetInfo* i,
                                                            XrActionStateVector2f* st) {
    XRSIM_ENTRY(impl_GetActionStateVector2f(s, i, st), "GetActionStateVector2f")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_GetActionStatePose(XrSession s, const XrActionStateGetInfo* i,
                                                        XrActionStatePose* st) {
    XRSIM_ENTRY(impl_GetActionStatePose(s, i, st), "GetActionStatePose")
}
XRAPI_ATTR XrResult XRAPI_CALL xrsim_SyncActions(XrSession s, const XrActionsSyncInfo* i) {
    XRSIM_ENTRY(impl_SyncActions(s, i), "SyncActions")
}

} // namespace xrsim
