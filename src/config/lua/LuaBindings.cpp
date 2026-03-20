#include "LuaBindings.hpp"
#include "ConfigManager.hpp"
#include "LuaEventHandler.hpp"
#include "devices/IKeyboard.hpp"
#include "objects/LuaWindow.hpp"
#include "objects/LuaWorkspace.hpp"
#include "objects/LuaMonitor.hpp"
#include "objects/LuaLayerSurface.hpp"
#include "types/LuaConfigFloat.hpp"
#include "types/LuaConfigInt.hpp"
#include "types/LuaConfigString.hpp"
#include "types/LuaConfigBool.hpp"

#include "../../Compositor.hpp"
#include "../../helpers/Monitor.hpp"
#include "../../managers/KeybindManager.hpp"
#include "../../managers/animation/AnimationManager.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"
#include "../../managers/input/trackpad/TrackpadGestures.hpp"
#include "../../managers/input/trackpad/gestures/DispatcherGesture.hpp"
#include "../../managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp"
#include "../../managers/input/trackpad/gestures/ResizeGesture.hpp"
#include "../../managers/input/trackpad/gestures/MoveGesture.hpp"
#include "../../managers/input/trackpad/gestures/SpecialWorkspaceGesture.hpp"
#include "../../managers/input/trackpad/gestures/CloseGesture.hpp"
#include "../../managers/input/trackpad/gestures/FloatGesture.hpp"
#include "../../managers/input/trackpad/gestures/FullscreenGesture.hpp"
#include "../../managers/input/trackpad/gestures/CursorZoomGesture.hpp"
#include "../supplementary/executor/Executor.hpp"
#include "../shared/animation/AnimationTree.hpp"

#include <hyprutils/string/String.hpp>
#include <hyprutils/string/VarList.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace Config::Lua::Bindings;
using namespace Config::Lua;
using namespace Hyprutils::String;

static void pushDispatcher(lua_State* L, const char* handler, std::string arg) {
    lua_newtable(L);
    lua_pushstring(L, handler);
    lua_setfield(L, -2, "_h");
    lua_pushstring(L, arg.c_str());
    lua_setfield(L, -2, "_a");
}

// converts a Lua string-or-number at stack position idx to std::string.
static std::string argStr(lua_State* L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER)
        return std::to_string((long long)lua_tonumber(L, idx));
    size_t      n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    return {s, n};
}

// returns def if the argument is absent or nil.
static std::string optStr(lua_State* L, int idx, const char* def = "") {
    if (lua_isnoneornil(L, idx))
        return def;
    return argStr(L, idx);
}

static std::optional<eKeyboardModifiers> modFromSv(std::string_view sv) {
    if (sv == "SHIFT")
        return HL_MODIFIER_SHIFT;
    if (sv == "CAPS")
        return HL_MODIFIER_CAPS;
    if (sv == "CTRL" || sv == "CONTROL")
        return HL_MODIFIER_CTRL;
    if (sv == "ALT" || sv == "MOD1")
        return HL_MODIFIER_ALT;
    if (sv == "MOD2")
        return HL_MODIFIER_MOD2;
    if (sv == "MOD3")
        return HL_MODIFIER_MOD3;
    if (sv == "SUPER" || sv == "WIN" || sv == "LOGO" || sv == "MOD4" || sv == "META")
        return HL_MODIFIER_META;
    if (sv == "MOD5")
        return HL_MODIFIER_MOD5;

    return std::nullopt;
}

static bool isSymSpecial(std::string_view sv) {
    if (sv == "mouse_down" || sv == "mouse_up" || sv == "mouse_left" || sv == "mouse_right")
        return true;

    return sv.starts_with("switch:") || sv.starts_with("mouse:");
}

// tries to parse a key string
static std::expected<void, std::string> parseKeyString(SKeybind& kb, std::string_view sv) {
    bool                      modsEnded = false, specialSym = false;

    CVarList2                 vl(sv, 0, '+', true);

    uint32_t                  modMask = 0;
    std::vector<xkb_keysym_t> keysyms;

    for (const auto& a : vl) {
        auto arg = Hyprutils::String::trim(a);

        auto mask = modFromSv(arg);

        if (!mask)
            modsEnded = true;

        if (modsEnded && mask)
            return std::unexpected("Modifiers must come first in the list");

        if (mask) {
            modMask |= *mask;
            continue;
        }

        if (specialSym)
            return std::unexpected("Cannot combine special syms (e.g. mouse_down + Q)");

        if (isSymSpecial(arg)) {
            if (!keysyms.empty())
                return std::unexpected("Cannot combine special syms (e.g. mouse_down + Q)");

            specialSym = true;
            kb.key     = arg;
            continue;
        }

        auto sym = xkb_keysym_from_name(std::string{arg}.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);

        if (sym == XKB_KEY_NoSymbol) {
            if (arg.contains(' '))
                return std::unexpected(std::format("Unknown keysym: \"{}\", did you forget a +?", arg));

            if (arg == "Enter")
                return std::unexpected(std::format(R"(Unknown keysym: "{}", did you mean "Return"?)", arg));

            return std::unexpected(std::format("Unknown keysym: \"{}\"", arg));
        }

        keysyms.emplace_back(sym);
    }

    kb.modmask = modMask;
    kb.sMkKeys = std::move(keysyms);

    return {};
}

static int hlBind(lua_State* L) {
    auto*            mgr = static_cast<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    std::string_view keys = luaL_checkstring(L, 1);

    SKeybind         kb;
    kb.submap.name = mgr->m_currentSubmap;

    if (auto res = parseKeyString(kb, keys); !res)
        return luaL_error(L, std::format("hl.bind: failed to parse key string: {}", res.error()).c_str());

    int optsIdx = 4;

    if (lua_isfunction(L, 2)) {
        // lua lambda as dispatcher
        lua_pushvalue(L, 2);
        int ref    = luaL_ref(L, LUA_REGISTRYINDEX);
        kb.handler = "__lua";
        kb.arg     = std::to_string(ref);
    } else if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "_h");
        lua_getfield(L, 2, "_a");

        if (!lua_isstring(L, -2)) {
            lua_pop(L, 2);
            return luaL_error(L, "hl.bind: invalid dispatcher table - missing handler");
        }

        kb.handler = lua_tostring(L, -2);
        kb.arg     = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 2);
    } else
        return luaL_error(L, "hl.bind: dispatcher must be a table returned by a dispatcher factory (e.g. hl.exec_cmd(), hl.window.close()) or a function");

    if (!g_pKeybindManager->m_dispatchers.contains(kb.handler))
        return luaL_error(L, "hl.bind: invalid dispatcher");

    if (lua_istable(L, optsIdx)) {
        auto getBool = [&](const char* field) -> bool {
            lua_getfield(L, optsIdx, field);
            bool v = lua_toboolean(L, -1);
            lua_pop(L, 1);
            return v;
        };
        kb.mouse           = getBool("mouse");
        kb.repeat          = getBool("repeating");
        kb.locked          = getBool("locked");
        kb.release         = getBool("release");
        kb.nonConsuming    = getBool("non_consuming");
        kb.transparent     = getBool("transparent");
        kb.ignoreMods      = getBool("ignore_mods");
        kb.dontInhibit     = getBool("dont_inhibit");
        kb.longPress       = getBool("long_press");
        kb.submapUniversal = getBool("submap_universal");

        bool click = false;
        bool drag  = false;

        if (getBool("click")) {
            click      = true;
            kb.release = true;
        }

        if (getBool("drag")) {
            drag       = true;
            kb.release = true;
        }

        if (click && drag)
            luaL_error(L, "hl.bind: click and drag are exclusive");

        if ((kb.longPress || kb.release) && kb.repeat)
            return luaL_error(L, "hl.bind: long_press / release is incompatible with repeat");

        if (kb.mouse && (kb.repeat || kb.release || kb.locked))
            return luaL_error(L, "hl.bind: mouse is exclusive");

        kb.click = click;
        kb.drag  = drag;

        if (kb.mouse)
            kb.handler = "mouse";

        lua_getfield(L, optsIdx, "device");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "inclusive");
            kb.deviceInclusive = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
            lua_pop(L, 1);

            lua_getfield(L, -1, "list");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (lua_isstring(L, -1))
                        kb.devices.emplace(lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    g_pKeybindManager->addKeybind(kb);
    return 0;
}

static int hlDefineSubmap(lua_State* L) {
    auto*       mgr  = static_cast<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    std::string prev     = mgr->m_currentSubmap;
    mgr->m_currentSubmap = name;

    lua_pushvalue(L, 2);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        mgr->addError(std::string("hl.define_submap: error in submap \"") + name + "\": " + lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    mgr->m_currentSubmap = prev;
    return 0;
}

static int hlExecCmd(lua_State* L) {
    pushDispatcher(L, "exec", argStr(L, 1));
    return 1;
}

static int hlExecRaw(lua_State* L) {
    pushDispatcher(L, "execr", argStr(L, 1));
    return 1;
}

static int hlExit(lua_State* L) {
    pushDispatcher(L, "exit", "");
    return 1;
}

static int hlSubmap(lua_State* L) {
    pushDispatcher(L, "submap", argStr(L, 1));
    return 1;
}

static int hlPass(lua_State* L) {
    pushDispatcher(L, "pass", argStr(L, 1));
    return 1;
}

static int hlSendShortcut(lua_State* L) {
    pushDispatcher(L, "sendshortcut", argStr(L, 1));
    return 1;
}

static int hlSendKeyState(lua_State* L) {
    pushDispatcher(L, "sendkeystate", argStr(L, 1));
    return 1;
}

static int hlLayout(lua_State* L) {
    pushDispatcher(L, "layoutmsg", argStr(L, 1));
    return 1;
}

static int hlDpms(lua_State* L) {
    std::string arg = argStr(L, 1);
    if (!lua_isnoneornil(L, 2))
        arg += " " + argStr(L, 2);
    pushDispatcher(L, "dpms", arg);
    return 1;
}

static int hlEvent(lua_State* L) {
    pushDispatcher(L, "event", argStr(L, 1));
    return 1;
}

static int hlGlobal(lua_State* L) {
    pushDispatcher(L, "global", argStr(L, 1));
    return 1;
}

static int hlForceRendererReload(lua_State* L) {
    pushDispatcher(L, "forcerendererreload", "");
    return 1;
}

static int hlForceIdle(lua_State* L) {
    pushDispatcher(L, "forceidle", "");
    return 1;
}

static int hlPrint(lua_State* L) {
    const int   n = lua_gettop(L);
    std::string out;
    for (int i = 1; i <= n; i++) {
        size_t      len = 0;
        const char* s   = luaL_tolstring(L, i, &len); // honours __tostring
        if (i > 1)
            out += '\t';
        out.append(s, len);
        lua_pop(L, 1);
    }
    Log::logger->log(Log::INFO, "[Lua] {}", out);
    return 0;
}

static int hlDispatch(lua_State* L) {
    if (!lua_istable(L, 1)) {
        luaL_error(L, "hl.dispatch: expected a dispatcher table (e.g. hl.exec_cmd(\"...\"))");
        return 0;
    }

    lua_getfield(L, 1, "_h");
    lua_getfield(L, 1, "_a");

    if (!lua_isstring(L, -2)) {
        luaL_error(L, "hl.dispatch: invalid dispatcher table - missing handler");
        return 0;
    }

    const std::string handler = lua_tostring(L, -2);
    const std::string arg     = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);

    const auto DISPATCHER = g_pKeybindManager->m_dispatchers.find(handler);
    if (DISPATCHER == g_pKeybindManager->m_dispatchers.end()) {
        luaL_error(L, "hl.dispatch: unknown dispatcher \"%s\"", handler.c_str());
        return 0;
    }

    DISPATCHER->second(arg);
    return 0;
}

static int hlGetWindows(lua_State* L) {
    lua_newtable(L);
    int i = 1;
    for (const auto& w : g_pCompositor->m_windows) {
        if (!w->m_isMapped)
            continue;
        Objects::CLuaWindow::push(L, w);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int hlGetWorkspaces(lua_State* L) {
    lua_newtable(L);
    int i = 1;
    for (const auto& wsRef : g_pCompositor->getWorkspaces()) {
        const auto ws = wsRef.lock();
        if (!ws || ws->inert())
            continue;
        Objects::CLuaWorkspace::push(L, ws);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int hlGetMonitors(lua_State* L) {
    lua_newtable(L);
    int i = 1;
    for (const auto& mon : g_pCompositor->m_monitors) {
        Objects::CLuaMonitor::push(L, mon);
        lua_rawseti(L, -2, i++);
    }
    return 1;
}

static int hlGetLayers(lua_State* L) {
    lua_newtable(L);
    int i = 1;
    for (const auto& mon : g_pCompositor->m_monitors) {
        for (const auto& level : mon->m_layerSurfaceLayers) {
            for (const auto& lsRef : level) {
                const auto ls = lsRef.lock();
                if (!ls)
                    continue;
                Objects::CLuaLayerSurface::push(L, ls);
                lua_rawseti(L, -2, i++);
            }
        }
    }
    return 1;
}

static int hlOn(lua_State* L) {
    auto*       mgr       = static_cast<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));
    const char* eventName = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (!mgr->m_eventHandler->registerEvent(eventName, ref)) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        const auto& known = CLuaEventHandler::knownEvents();
        std::string list;
        for (const auto& e : known)
            list += "\n  " + e;
        return luaL_error(L, "%s", (std::string("hl.on: unknown event \"") + eventName + "\". Known events:" + list).c_str());
    }

    return 0;
}

static int hlWindowClose(lua_State* L) {
    pushDispatcher(L, "killactive", "");
    return 1;
}

static int hlWindowForceClose(lua_State* L) {
    pushDispatcher(L, "forcekillactive", "");
    return 1;
}

static int hlWindowCloseWindow(lua_State* L) {
    pushDispatcher(L, "closewindow", optStr(L, 1));
    return 1;
}

static int hlWindowKillWindow(lua_State* L) {
    pushDispatcher(L, "killwindow", optStr(L, 1));
    return 1;
}

static int hlWindowSignal(lua_State* L) {
    pushDispatcher(L, "signal", argStr(L, 1));
    return 1;
}

static int hlWindowSignalWindow(lua_State* L) {
    pushDispatcher(L, "signalwindow", argStr(L, 1) + "," + argStr(L, 2));
    return 1;
}

static int hlWindowToggleFloat(lua_State* L) {
    pushDispatcher(L, "togglefloating", optStr(L, 1));
    return 1;
}

static int hlWindowSetFloat(lua_State* L) {
    pushDispatcher(L, "setfloating", optStr(L, 1));
    return 1;
}

static int hlWindowSetTiled(lua_State* L) {
    pushDispatcher(L, "settiled", optStr(L, 1));
    return 1;
}

static int hlWindowFullscreen(lua_State* L) {
    pushDispatcher(L, "fullscreen", optStr(L, 1, "0"));
    return 1;
}

static int hlWindowFullscreenState(lua_State* L) {
    pushDispatcher(L, "fullscreenstate", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlWindowPseudo(lua_State* L) {
    pushDispatcher(L, "pseudo", "");
    return 1;
}

static int hlWindowMove(lua_State* L) {
    pushDispatcher(L, "movewindow", optStr(L, 1));
    return 1;
}

static int hlWindowSwap(lua_State* L) {
    pushDispatcher(L, "swapwindow", argStr(L, 1));
    return 1;
}

static int hlWindowCenter(lua_State* L) {
    pushDispatcher(L, "centerwindow", optStr(L, 1));
    return 1;
}

static int hlWindowCycleNext(lua_State* L) {
    pushDispatcher(L, "cyclenext", optStr(L, 1));
    return 1;
}

static int hlWindowSwapNext(lua_State* L) {
    pushDispatcher(L, "swapnext", optStr(L, 1));
    return 1;
}

static int hlWindowFocus(lua_State* L) {
    pushDispatcher(L, "focuswindow", argStr(L, 1));
    return 1;
}

static int hlWindowFocusByClass(lua_State* L) {
    pushDispatcher(L, "focuswindowbyclass", argStr(L, 1));
    return 1;
}

static int hlWindowTag(lua_State* L) {
    std::string arg = argStr(L, 1);
    if (!lua_isnoneornil(L, 2))
        arg += " " + argStr(L, 2);
    pushDispatcher(L, "tagwindow", arg);
    return 1;
}

static int hlWindowToggleSwallow(lua_State* L) {
    pushDispatcher(L, "toggleswallow", "");
    return 1;
}

static int hlWindowResizeActive(lua_State* L) {
    pushDispatcher(L, "resizeactive", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlWindowMoveActive(lua_State* L) {
    pushDispatcher(L, "moveactive", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlWindowResizePixel(lua_State* L) {
    pushDispatcher(L, "resizewindowpixel", argStr(L, 1) + "," + argStr(L, 2));
    return 1;
}

static int hlWindowMovePixel(lua_State* L) {
    pushDispatcher(L, "movewindowpixel", argStr(L, 1) + "," + argStr(L, 2));
    return 1;
}

static int hlWindowPin(lua_State* L) {
    pushDispatcher(L, "pin", "");
    return 1;
}

static int hlWindowBringToTop(lua_State* L) {
    pushDispatcher(L, "bringactivetotop", "");
    return 1;
}

static int hlWindowAlterZOrder(lua_State* L) {
    pushDispatcher(L, "alterzorder", argStr(L, 1));
    return 1;
}

static int hlWindowSetProp(lua_State* L) {
    pushDispatcher(L, "setprop", argStr(L, 1) + " " + argStr(L, 2) + " " + argStr(L, 3));
    return 1;
}

static int hlWindowMoveIntoGroup(lua_State* L) {
    pushDispatcher(L, "moveintogroup", argStr(L, 1));
    return 1;
}

static int hlWindowMoveOutOfGroup(lua_State* L) {
    pushDispatcher(L, "moveoutofgroup", optStr(L, 1));
    return 1;
}

static int hlWindowMoveWindowOrGroup(lua_State* L) {
    pushDispatcher(L, "movewindoworgroup", argStr(L, 1));
    return 1;
}

static int hlWindowDenyFromGroup(lua_State* L) {
    pushDispatcher(L, "denywindowfromgroup", optStr(L, 1, "toggle"));
    return 1;
}

static int hlWindowDrag(lua_State* L) {
    pushDispatcher(L, "mouse", "movewindow");
    return 1;
}

static int hlWindowResize(lua_State* L) {
    pushDispatcher(L, "mouse", "resizewindow");
    return 1;
}

static int hlFocusDirection(lua_State* L) {
    pushDispatcher(L, "movefocus", argStr(L, 1));
    return 1;
}

static int hlFocusMonitor(lua_State* L) {
    pushDispatcher(L, "focusmonitor", argStr(L, 1));
    return 1;
}

static int hlFocusUrgentOrLast(lua_State* L) {
    pushDispatcher(L, "focusurgentorlast", "");
    return 1;
}

static int hlFocusCurrentOrLast(lua_State* L) {
    pushDispatcher(L, "focuscurrentorlast", "");
    return 1;
}

static int hlWorkspaceGo(lua_State* L) {
    pushDispatcher(L, "workspace", argStr(L, 1));
    return 1;
}

static int hlWorkspaceMoveWindow(lua_State* L) {
    pushDispatcher(L, "movetoworkspace", argStr(L, 1));
    return 1;
}

static int hlWorkspaceMoveWindowSilent(lua_State* L) {
    pushDispatcher(L, "movetoworkspacesilent", argStr(L, 1));
    return 1;
}

static int hlWorkspaceSpecial(lua_State* L) {
    pushDispatcher(L, "togglespecialworkspace", optStr(L, 1));
    return 1;
}

static int hlWorkspaceRename(lua_State* L) {
    pushDispatcher(L, "renameworkspace", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlWorkspaceMoveToMonitor(lua_State* L) {
    pushDispatcher(L, "moveworkspacetomonitor", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlWorkspaceMoveCurrentToMonitor(lua_State* L) {
    pushDispatcher(L, "movecurrentworkspacetomonitor", argStr(L, 1));
    return 1;
}

static int hlWorkspaceFocusOnMonitor(lua_State* L) {
    pushDispatcher(L, "focusworkspaceoncurrentmonitor", argStr(L, 1));
    return 1;
}

static int hlWorkspaceSwapMonitors(lua_State* L) {
    pushDispatcher(L, "swapactiveworkspaces", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlCursorMoveToCorner(lua_State* L) {
    pushDispatcher(L, "movecursortocorner", argStr(L, 1));
    return 1;
}

static int hlCursorMove(lua_State* L) {
    pushDispatcher(L, "movecursor", argStr(L, 1) + " " + argStr(L, 2));
    return 1;
}

static int hlGroupToggle(lua_State* L) {
    pushDispatcher(L, "togglegroup", "");
    return 1;
}

static int hlGroupChangeActive(lua_State* L) {
    pushDispatcher(L, "changegroupactive", argStr(L, 1));
    return 1;
}

static int hlGroupMoveWindow(lua_State* L) {
    pushDispatcher(L, "movegroupwindow", argStr(L, 1));
    return 1;
}

static int hlGroupLock(lua_State* L) {
    pushDispatcher(L, "lockgroups", optStr(L, 1, "toggle"));
    return 1;
}

static int hlGroupLockActive(lua_State* L) {
    pushDispatcher(L, "lockactivegroup", optStr(L, 1, "toggle"));
    return 1;
}

static int hlGroupIgnoreLock(lua_State* L) {
    pushDispatcher(L, "setignoregrouplock", optStr(L, 1, "toggle"));
    return 1;
}

static int hlExecOnce(lua_State* L) {
    auto* mgr = static_cast<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (mgr->isFirstLaunch())
        Config::Supplementary::executor()->addExecOnce({argStr(L, 1), true});

    return 0;
}

static int hlExecShutdown(lua_State* L) {
    if (g_pCompositor->m_finalRequests) {
        Config::Supplementary::executor()->spawn(argStr(L, 1));
        return 0;
    }

    Config::Supplementary::executor()->addExecShutdown({argStr(L, 1), true});
    return 0;
}

// push table field onto the stack, parse it with a typed parser, pop, and return the error (if any).
// on success the parsed value lives inside `parser`
template <typename T>
static SParseError parseTableField(lua_State* L, int tableIdx, const char* field, T& parser) {
    lua_getfield(L, tableIdx, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return {.errorCode = PARSE_ERROR_BAD_VALUE, .message = std::format("missing required field \"{}\"", field)};
    }
    auto err = parser.parse(L);
    lua_pop(L, 1);
    if (err.errorCode != PARSE_ERROR_OK)
        err.message = std::format("field \"{}\": {}", field, err.message);
    return err;
}

static int hlCurve(lua_State* L) {
    CLuaConfigString nameParser("");
    lua_pushvalue(L, 1);
    auto nameErr = nameParser.parse(L);
    lua_pop(L, 1);
    if (nameErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.curve: first argument (name) must be a string: {}", nameErr.message).c_str());

    const auto& name = nameParser.parsed();

    if (!lua_istable(L, 2))
        return luaL_error(L, "hl.curve: second argument must be a table, e.g. { type = \"bezier\", points = { {0, 0}, {1, 1} } }");

    // parse type field
    CLuaConfigString typeParser("");
    auto             typeErr = parseTableField(L, 2, "type", typeParser);
    if (typeErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.curve(\"{}\"): {}", name, typeErr.message).c_str());

    const auto& curveType = typeParser.parsed();

    if (curveType != "bezier")
        return luaL_error(L, "%s", std::format("hl.curve(\"{}\"): unknown curve type \"{}\", expected \"bezier\"", name, curveType).c_str());

    // parse points field - must be a table of two sub-tables: { {p1x, p1y}, {p2x, p2y} }
    lua_getfield(L, 2, "points");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "%s", std::format("hl.curve(\"{}\"): missing or invalid \"points\" field, expected a table of two points", name).c_str());
    }
    int pointsIdx = lua_gettop(L);

    if (luaL_len(L, pointsIdx) != 2) {
        lua_pop(L, 1);
        return luaL_error(L, "%s", std::format("hl.curve(\"{}\"): \"points\" must contain exactly 2 points, e.g. {{ {{0, 0}}, {{1, 1}} }}", name).c_str());
    }

    float coords[4] = {};
    for (int pt = 1; pt <= 2; pt++) {
        lua_rawgeti(L, pointsIdx, pt);
        if (!lua_istable(L, -1) || luaL_len(L, -1) != 2) {
            lua_pop(L, 2); // point + points table
            return luaL_error(L, "%s", std::format("hl.curve(\"{}\"): point {} must be a table of 2 numbers, e.g. {{0.25, 0.1}}", name, pt).c_str());
        }
        int ptIdx = lua_gettop(L);

        for (int comp = 0; comp < 2; comp++) {
            lua_rawgeti(L, ptIdx, comp + 1);
            CLuaConfigFloat coordParser(0.F, -1.F, 2.F);
            auto            coordErr = coordParser.parse(L);
            lua_pop(L, 1);
            if (coordErr.errorCode != PARSE_ERROR_OK) {
                lua_pop(L, 2); // point + points table
                return luaL_error(L, "%s", std::format("hl.curve(\"{}\"): point {}[{}]: {}", name, pt, comp + 1, coordErr.message).c_str());
            }
            coords[((pt - 1) * 2) + comp] = coordParser.parsed();
        }

        lua_pop(L, 1); // pop point table
    }
    lua_pop(L, 1); // pop points table

    g_pAnimationManager->addBezierWithName(name, Vector2D(coords[0], coords[1]), Vector2D(coords[2], coords[3]));
    return 0;
}

static int hlAnimation(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "hl.animation: expected a table, e.g. { leaf = \"global\", enabled = true, speed = 5, bezier = \"default\" }");

    // parse leaf
    CLuaConfigString leafParser("");
    auto             leafErr = parseTableField(L, 1, "leaf", leafParser);
    if (leafErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.animation: {}", leafErr.message).c_str());

    const auto leaf = leafParser.parsed();

    if (!Config::animationTree()->nodeExists(leaf))
        return luaL_error(L, "%s", std::format("hl.animation: no such animation leaf \"{}\"", leaf).c_str());

    // parse enabled
    CLuaConfigBool enabledParser(true);
    auto           enabledErr = parseTableField(L, 1, "enabled", enabledParser);
    if (enabledErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): {}", leaf, enabledErr.message).c_str());

    bool enabled = enabledParser.parsed();

    if (!enabled) {
        Config::animationTree()->setConfigForNode(leaf, false, 1, "default");
        return 0;
    }

    // parse speed
    CLuaConfigFloat speedParser(0.F, 0.F, 100.F);
    auto            speedErr = parseTableField(L, 1, "speed", speedParser);
    if (speedErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): {}", leaf, speedErr.message).c_str());

    float speed = speedParser.parsed();

    if (speed <= 0)
        return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): speed must be greater than 0", leaf).c_str());

    // parse bezier
    CLuaConfigString bezierParser("");
    auto             bezierErr = parseTableField(L, 1, "bezier", bezierParser);
    if (bezierErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): {}", leaf, bezierErr.message).c_str());

    const auto& bezierName = bezierParser.parsed();

    if (!g_pAnimationManager->bezierExists(bezierName))
        return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): no such bezier \"{}\"", leaf, bezierName).c_str());

    // parse optional style
    std::string style;
    lua_getfield(L, 1, "style");
    if (!lua_isnil(L, -1)) {
        CLuaConfigString styleParser("");
        auto             styleErr = styleParser.parse(L);
        if (styleErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): field \"style\": {}", leaf, styleErr.message).c_str());
        }
        style = styleParser.parsed();
    }
    lua_pop(L, 1);

    if (!style.empty()) {
        auto err = g_pAnimationManager->styleValidInConfigVar(leaf, style);
        if (!err.empty()) {
            return luaL_error(L, "%s", std::format("hl.animation(\"{}\"): {}", leaf, err).c_str());
        }
    }

    Config::animationTree()->setConfigForNode(leaf, true, speed, bezierName, style);
    return 0;
}

static int hlUnbind(lua_State* L) {
    // hl.unbind("all") clears everything
    if (lua_isstring(L, 1) && std::string_view(lua_tostring(L, 1)) == "all" && lua_gettop(L) == 1) {
        g_pKeybindManager->clearKeybinds();
        return 0;
    }

    const char* mods   = luaL_checkstring(L, 1);
    const char* keyStr = luaL_checkstring(L, 2);

    uint32_t    mod = g_pKeybindManager->stringToModMask(mods);

    SParsedKey  key;
    std::string k = keyStr;
    if (Hyprutils::String::isNumber(k) && std::stoi(k) > 9)
        key = {.keycode = (uint32_t)std::stoi(k)};
    else if (k.starts_with("code:") && Hyprutils::String::isNumber(k.substr(5)))
        key = {.keycode = (uint32_t)std::stoi(k.substr(5))};
    else if (k == "catchall")
        key = {.catchAll = true};
    else
        key = {.key = k};

    g_pKeybindManager->removeKeybind(mod, key);
    return 0;
}

static int hlTimer(lua_State* L) {
    auto* mgr = static_cast<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    luaL_checktype(L, 1, LUA_TFUNCTION);
    luaL_checktype(L, 2, LUA_TTABLE);

    // read opts.timeout
    lua_getfield(L, 2, "timeout");
    if (!lua_isnumber(L, -1))
        return luaL_error(L, "hl.timer: opts.timeout must be a number (ms)");
    int timeoutMs = (int)lua_tonumber(L, -1);
    lua_pop(L, 1);

    if (timeoutMs <= 0)
        return luaL_error(L, "hl.timer: opts.timeout must be > 0");

    // read opts.type
    lua_getfield(L, 2, "type");
    if (!lua_isstring(L, -1))
        return luaL_error(L, "hl.timer: opts.type must be \"repeat\" or \"oneshot\"");
    std::string type = lua_tostring(L, -1);
    lua_pop(L, 1);

    bool repeat = false;
    if (type == "repeat")
        repeat = true;
    else if (type != "oneshot")
        return luaL_error(L, "hl.timer: opts.type must be \"repeat\" or \"oneshot\"");

    // store the lua callback
    lua_pushvalue(L, 1);
    int  ref = luaL_ref(L, LUA_REGISTRYINDEX);

    auto timer = makeShared<CEventLoopTimer>(
        std::chrono::milliseconds(timeoutMs),
        [L, ref, repeat, timeoutMs, mgr](SP<CEventLoopTimer> self, void* data) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                Log::logger->log(Log::ERR, "[Lua] error in timer callback: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }

            if (repeat)
                self->updateTimeout(std::chrono::milliseconds(timeoutMs));
            else {
                luaL_unref(L, LUA_REGISTRYINDEX, ref);
                std::erase_if(mgr->m_luaTimers, [&self](const auto& lt) { return lt.timer == self; });
            }
        },
        nullptr);

    mgr->m_luaTimers.emplace_back(CConfigManager::SLuaTimer{timer, ref});
    g_pEventLoopManager->addTimer(timer);

    return 0;
}

static int hlEnv(lua_State* L) {
    auto*            mgr = static_cast<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    CLuaConfigString nameParser("");
    lua_pushvalue(L, 1);
    auto nameErr = nameParser.parse(L);
    lua_pop(L, 1);
    if (nameErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.env: first argument (name) must be a string: {}", nameErr.message).c_str());

    const auto& name = nameParser.parsed();

    if (name.empty())
        return luaL_error(L, "hl.env: name must not be empty");

    CLuaConfigString valueParser("");
    lua_pushvalue(L, 2);
    auto valueErr = valueParser.parse(L);
    lua_pop(L, 1);
    if (valueErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.env: second argument (value) must be a string: {}", valueErr.message).c_str());

    const auto& value = valueParser.parsed();

    if (!mgr->isFirstLaunch()) {
        const auto* ENV = getenv(name.c_str());
        if (ENV && ENV == value)
            return 0;
    }

    setenv(name.c_str(), value.c_str(), 1);

    // optional third argument: propagate to dbus/systemd
    bool dbus = false;
    if (!lua_isnoneornil(L, 3)) {
        CLuaConfigBool dbusParser(false);
        lua_pushvalue(L, 3);
        auto dbusErr = dbusParser.parse(L);
        lua_pop(L, 1);
        if (dbusErr.errorCode != PARSE_ERROR_OK)
            return luaL_error(L, "%s", std::format("hl.env: third argument (dbus) must be a boolean: {}", dbusErr.message).c_str());

        dbus = dbusParser.parsed();
    }

    if (dbus) {
        std::string CMD;
#ifdef USES_SYSTEMD
        CMD = "systemctl --user import-environment " + name + " && hash dbus-update-activation-environment 2>/dev/null && ";
#endif
        CMD += "dbus-update-activation-environment --systemd " + name;
        if (mgr->isFirstLaunch())
            Config::Supplementary::executor()->addExecOnce({CMD, false});
        else
            Config::Supplementary::executor()->spawnRaw(CMD);
    }

    return 0;
}

static int hlGesture(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "hl.gesture: expected a table, e.g. { fingers = 3, direction = \"horizontal\", action = \"workspace\" }");

    // parse fingers
    CLuaConfigInt fingersParser(0, 2, 9);
    auto          fingersErr = parseTableField(L, 1, "fingers", fingersParser);
    if (fingersErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.gesture: {}", fingersErr.message).c_str());

    size_t fingerCount = fingersParser.parsed();

    // parse direction
    CLuaConfigString dirParser("");
    auto             dirErr = parseTableField(L, 1, "direction", dirParser);
    if (dirErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.gesture: {}", dirErr.message).c_str());

    const auto direction = g_pTrackpadGestures->dirForString(dirParser.parsed());
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return luaL_error(L, "%s", std::format("hl.gesture: invalid direction \"{}\"", dirParser.parsed()).c_str());

    // parse action
    CLuaConfigString actionParser("");
    auto             actionErr = parseTableField(L, 1, "action", actionParser);
    if (actionErr.errorCode != PARSE_ERROR_OK)
        return luaL_error(L, "%s", std::format("hl.gesture: {}", actionErr.message).c_str());

    const auto& action = actionParser.parsed();

    // parse optional mods
    uint32_t modMask = 0;
    lua_getfield(L, 1, "mods");
    if (!lua_isnil(L, -1)) {
        CLuaConfigString modsParser("");
        auto             modsErr = modsParser.parse(L);
        if (modsErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return luaL_error(L, "%s", std::format("hl.gesture: field \"mods\": {}", modsErr.message).c_str());
        }
        modMask = g_pKeybindManager->stringToModMask(modsParser.parsed());
    }
    lua_pop(L, 1);

    // parse optional scale (clamped 0.1 - 10)
    float deltaScale = 1.F;
    lua_getfield(L, 1, "scale");
    if (!lua_isnil(L, -1)) {
        CLuaConfigFloat scaleParser(1.F, 0.1F, 10.F);
        auto            scaleErr = scaleParser.parse(L);
        if (scaleErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return luaL_error(L, "%s", std::format("hl.gesture: field \"scale\": {}", scaleErr.message).c_str());
        }
        deltaScale = scaleParser.parsed();
    }
    lua_pop(L, 1);

    // parse optional arg (for dispatcher, special, float, fullscreen, cursorZoom)
    std::string actionArg;
    lua_getfield(L, 1, "arg");
    if (!lua_isnil(L, -1)) {
        CLuaConfigString argParser("");
        auto             argErr = argParser.parse(L);
        if (argErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return luaL_error(L, "%s", std::format("hl.gesture: field \"arg\": {}", argErr.message).c_str());
        }
        actionArg = argParser.parsed();
    }
    lua_pop(L, 1);

    // parse optional arg2 (for cursorZoom second arg)
    std::string actionArg2;
    lua_getfield(L, 1, "arg2");
    if (!lua_isnil(L, -1)) {
        CLuaConfigString arg2Parser("");
        auto             arg2Err = arg2Parser.parse(L);
        if (arg2Err.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return luaL_error(L, "%s", std::format("hl.gesture: field \"arg2\": {}", arg2Err.message).c_str());
        }
        actionArg2 = arg2Parser.parsed();
    }
    lua_pop(L, 1);

    constexpr bool                   disableInhibit = false;

    std::expected<void, std::string> result;

    if (action == "dispatcher")
        result = g_pTrackpadGestures->addGesture(makeUnique<CDispatcherTrackpadGesture>(actionArg, actionArg2), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "workspace")
        result = g_pTrackpadGestures->addGesture(makeUnique<CWorkspaceSwipeGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "resize")
        result = g_pTrackpadGestures->addGesture(makeUnique<CResizeTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "move")
        result = g_pTrackpadGestures->addGesture(makeUnique<CMoveTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "special")
        result = g_pTrackpadGestures->addGesture(makeUnique<CSpecialWorkspaceGesture>(actionArg), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "close")
        result = g_pTrackpadGestures->addGesture(makeUnique<CCloseTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "float")
        result = g_pTrackpadGestures->addGesture(makeUnique<CFloatTrackpadGesture>(actionArg), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "fullscreen")
        result = g_pTrackpadGestures->addGesture(makeUnique<CFullscreenTrackpadGesture>(actionArg), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "cursorZoom")
        result = g_pTrackpadGestures->addGesture(makeUnique<CCursorZoomTrackpadGesture>(actionArg, actionArg2), fingerCount, direction, modMask, deltaScale, disableInhibit);
    else if (action == "unset")
        result = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    else
        return luaL_error(L, "%s", std::format("hl.gesture: unknown action \"{}\"", action).c_str());

    if (!result)
        return luaL_error(L, "%s", std::format("hl.gesture: {}", result.error()).c_str());

    return 0;
}

void Bindings::registerBindings(lua_State* L, CConfigManager* mgr) {
    // register a __lua dispatcher for lua lambda keybinds
    g_pKeybindManager->m_dispatchers["__lua"] = [L](std::string arg) -> SDispatchResult {
        int ref = std::stoi(arg);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            Log::logger->log(Log::ERR, "[Lua] error in keybind lambda: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        return {};
    };

    lua_getglobal(L, "hl");

    // hl.on
    lua_pushlightuserdata(L, mgr);
    lua_pushcclosure(L, hlOn, 1);
    lua_setfield(L, -2, "on");

    // hl.bind
    lua_pushlightuserdata(L, mgr);
    lua_pushcclosure(L, hlBind, 1);
    lua_setfield(L, -2, "bind");

    // hl.define_submap
    lua_pushlightuserdata(L, mgr);
    lua_pushcclosure(L, hlDefineSubmap, 1);
    lua_setfield(L, -2, "define_submap");

    // hl.timer
    lua_pushlightuserdata(L, mgr);
    lua_pushcclosure(L, hlTimer, 1);
    lua_setfield(L, -2, "timer");

    // hl.dispatch
    lua_pushcfunction(L, hlDispatch);
    lua_setfield(L, -2, "dispatch");

    // Top-level dispatcher factories
    lua_pushcfunction(L, hlExecCmd);
    lua_setfield(L, -2, "exec_cmd");
    lua_pushcfunction(L, hlExecRaw);
    lua_setfield(L, -2, "exec_raw");
    lua_pushcfunction(L, hlExit);
    lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, hlSubmap);
    lua_setfield(L, -2, "submap");
    lua_pushcfunction(L, hlPass);
    lua_setfield(L, -2, "pass");
    lua_pushcfunction(L, hlSendShortcut);
    lua_setfield(L, -2, "send_shortcut");
    lua_pushcfunction(L, hlSendKeyState);
    lua_setfield(L, -2, "send_key_state");
    lua_pushcfunction(L, hlLayout);
    lua_setfield(L, -2, "layout");
    lua_pushcfunction(L, hlDpms);
    lua_setfield(L, -2, "dpms");
    lua_pushcfunction(L, hlEvent);
    lua_setfield(L, -2, "event");
    lua_pushcfunction(L, hlGlobal);
    lua_setfield(L, -2, "global");
    lua_pushcfunction(L, hlForceRendererReload);
    lua_setfield(L, -2, "force_renderer_reload");
    lua_pushcfunction(L, hlForceIdle);
    lua_setfield(L, -2, "force_idle");

    // hl.env
    lua_pushlightuserdata(L, mgr);
    lua_pushcclosure(L, hlEnv, 1);
    lua_setfield(L, -2, "env");

    // hl.gesture
    lua_pushcfunction(L, hlGesture);
    lua_setfield(L, -2, "gesture");

    // hl.exec_once
    lua_pushlightuserdata(L, mgr);
    lua_pushcclosure(L, hlExecOnce, 1);
    lua_setfield(L, -2, "exec_once");

    // hl.exec_shutdown
    lua_pushcfunction(L, hlExecShutdown);
    lua_setfield(L, -2, "exec_shutdown");

    // hl.curve
    lua_pushcfunction(L, hlCurve);
    lua_setfield(L, -2, "curve");

    // hl.animation
    lua_pushcfunction(L, hlAnimation);
    lua_setfield(L, -2, "animation");

    // hl.unbind
    lua_pushcfunction(L, hlUnbind);
    lua_setfield(L, -2, "unbind");

    lua_pushcfunction(L, hlGetWindows);
    lua_setfield(L, -2, "get_windows");
    lua_pushcfunction(L, hlGetWorkspaces);
    lua_setfield(L, -2, "get_workspaces");
    lua_pushcfunction(L, hlGetMonitors);
    lua_setfield(L, -2, "get_monitors");
    lua_pushcfunction(L, hlGetLayers);
    lua_setfield(L, -2, "get_layers");

    // hl.window
    lua_newtable(L);
    lua_pushcfunction(L, hlWindowClose);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, hlWindowForceClose);
    lua_setfield(L, -2, "force_close");
    lua_pushcfunction(L, hlWindowCloseWindow);
    lua_setfield(L, -2, "close_window");
    lua_pushcfunction(L, hlWindowKillWindow);
    lua_setfield(L, -2, "kill_window");
    lua_pushcfunction(L, hlWindowSignal);
    lua_setfield(L, -2, "signal");
    lua_pushcfunction(L, hlWindowSignalWindow);
    lua_setfield(L, -2, "signal_window");
    lua_pushcfunction(L, hlWindowToggleFloat);
    lua_setfield(L, -2, "toggle_float");
    lua_pushcfunction(L, hlWindowSetFloat);
    lua_setfield(L, -2, "set_float");
    lua_pushcfunction(L, hlWindowSetTiled);
    lua_setfield(L, -2, "set_tiled");
    lua_pushcfunction(L, hlWindowFullscreen);
    lua_setfield(L, -2, "fullscreen");
    lua_pushcfunction(L, hlWindowFullscreenState);
    lua_setfield(L, -2, "fullscreen_state");
    lua_pushcfunction(L, hlWindowPseudo);
    lua_setfield(L, -2, "pseudo");
    lua_pushcfunction(L, hlWindowMove);
    lua_setfield(L, -2, "move");
    lua_pushcfunction(L, hlWindowSwap);
    lua_setfield(L, -2, "swap");
    lua_pushcfunction(L, hlWindowCenter);
    lua_setfield(L, -2, "center");
    lua_pushcfunction(L, hlWindowCycleNext);
    lua_setfield(L, -2, "cycle_next");
    lua_pushcfunction(L, hlWindowSwapNext);
    lua_setfield(L, -2, "swap_next");
    lua_pushcfunction(L, hlWindowFocus);
    lua_setfield(L, -2, "focus");
    lua_pushcfunction(L, hlWindowFocusByClass);
    lua_setfield(L, -2, "focus_by_class");
    lua_pushcfunction(L, hlWindowTag);
    lua_setfield(L, -2, "tag");
    lua_pushcfunction(L, hlWindowToggleSwallow);
    lua_setfield(L, -2, "toggle_swallow");
    lua_pushcfunction(L, hlWindowResizeActive);
    lua_setfield(L, -2, "resize_active");
    lua_pushcfunction(L, hlWindowMoveActive);
    lua_setfield(L, -2, "move_active");
    lua_pushcfunction(L, hlWindowResizePixel);
    lua_setfield(L, -2, "resize_pixel");
    lua_pushcfunction(L, hlWindowMovePixel);
    lua_setfield(L, -2, "move_pixel");
    lua_pushcfunction(L, hlWindowPin);
    lua_setfield(L, -2, "pin");
    lua_pushcfunction(L, hlWindowBringToTop);
    lua_setfield(L, -2, "bring_to_top");
    lua_pushcfunction(L, hlWindowAlterZOrder);
    lua_setfield(L, -2, "alter_zorder");
    lua_pushcfunction(L, hlWindowSetProp);
    lua_setfield(L, -2, "set_prop");
    lua_pushcfunction(L, hlWindowMoveIntoGroup);
    lua_setfield(L, -2, "move_into_group");
    lua_pushcfunction(L, hlWindowMoveOutOfGroup);
    lua_setfield(L, -2, "move_out_of_group");
    lua_pushcfunction(L, hlWindowMoveWindowOrGroup);
    lua_setfield(L, -2, "move_window_or_group");
    lua_pushcfunction(L, hlWindowDenyFromGroup);
    lua_setfield(L, -2, "deny_from_group");
    lua_pushcfunction(L, hlWindowDrag);
    lua_setfield(L, -2, "drag");
    lua_pushcfunction(L, hlWindowResize);
    lua_setfield(L, -2, "resize");
    lua_setfield(L, -2, "window");

    // hl.focus
    lua_newtable(L);
    lua_pushcfunction(L, hlFocusDirection);
    lua_setfield(L, -2, "direction");
    lua_pushcfunction(L, hlFocusMonitor);
    lua_setfield(L, -2, "monitor");
    lua_pushcfunction(L, hlFocusUrgentOrLast);
    lua_setfield(L, -2, "urgent_or_last");
    lua_pushcfunction(L, hlFocusCurrentOrLast);
    lua_setfield(L, -2, "current_or_last");
    lua_setfield(L, -2, "focus");

    // hl.workspace
    lua_newtable(L);
    lua_pushcfunction(L, hlWorkspaceGo);
    lua_setfield(L, -2, "go");
    lua_pushcfunction(L, hlWorkspaceMoveWindow);
    lua_setfield(L, -2, "move_window");
    lua_pushcfunction(L, hlWorkspaceMoveWindowSilent);
    lua_setfield(L, -2, "move_window_silent");
    lua_pushcfunction(L, hlWorkspaceSpecial);
    lua_setfield(L, -2, "special");
    lua_pushcfunction(L, hlWorkspaceRename);
    lua_setfield(L, -2, "rename");
    lua_pushcfunction(L, hlWorkspaceMoveToMonitor);
    lua_setfield(L, -2, "move_to_monitor");
    lua_pushcfunction(L, hlWorkspaceMoveCurrentToMonitor);
    lua_setfield(L, -2, "move_current_to_monitor");
    lua_pushcfunction(L, hlWorkspaceFocusOnMonitor);
    lua_setfield(L, -2, "focus_on_monitor");
    lua_pushcfunction(L, hlWorkspaceSwapMonitors);
    lua_setfield(L, -2, "swap_monitors");
    lua_setfield(L, -2, "workspace");

    // hl.cursor
    lua_newtable(L);
    lua_pushcfunction(L, hlCursorMoveToCorner);
    lua_setfield(L, -2, "move_to_corner");
    lua_pushcfunction(L, hlCursorMove);
    lua_setfield(L, -2, "move");
    lua_setfield(L, -2, "cursor");

    // hl.group
    lua_newtable(L);
    lua_pushcfunction(L, hlGroupToggle);
    lua_setfield(L, -2, "toggle");
    lua_pushcfunction(L, hlGroupChangeActive);
    lua_setfield(L, -2, "change_active");
    lua_pushcfunction(L, hlGroupMoveWindow);
    lua_setfield(L, -2, "move_window");
    lua_pushcfunction(L, hlGroupLock);
    lua_setfield(L, -2, "lock");
    lua_pushcfunction(L, hlGroupLockActive);
    lua_setfield(L, -2, "lock_active");
    lua_pushcfunction(L, hlGroupIgnoreLock);
    lua_setfield(L, -2, "ignore_lock");
    lua_setfield(L, -2, "group");

    lua_pop(L, 1); // pop hl

    // override the global print() to route through the Hyprland logger.
    lua_pushcfunction(L, hlPrint);
    lua_setglobal(L, "print");
}
