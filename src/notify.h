#pragma once

// Windows notifications from a plain (unpackaged) exe, for the low disk space alerts.
//
// A toast needs an AppUserModelID that Windows knows: registered per user under
// HKCU\Software\Classes\AppUserModelId\<id> (DisplayName, IconUri), which works without an MSIX
// package or a Start menu shortcut. Clicking a toast opens a link: the bytopsy: protocol,
// registered under HKCU\Software\Classes\bytopsy, starts bytopsy.exe --alert-link "<link>".
// Both are written when alerts are turned on and removed when they are turned off.
// If toasts can't be shown at all (the WinRT notification API is missing or refuses), a
// notification-area balloon is the fallback.

#include <string>
#include <string_view>
#include <vector>

// ---- The toast's XML (pure, testable)

struct ToastAction {
    std::wstring label;
    std::wstring link;  // protocol activation
};

struct ToastContent {
    std::wstring title;
    std::wstring body;
    std::wstring footer;  // smaller attribution text; may be empty
    std::wstring link;    // where a click on the toast itself goes (protocol activation); may be empty
    std::vector<ToastAction> actions;
};

// & < > " ' as entities; characters XML 1.0 forbids (control characters, lone surrogates) dropped.
std::wstring EscapeXml(std::wstring_view s);
// <toast launch=... activationType="protocol"><visual><binding template="ToastGeneric">...
std::wstring BuildToastXml(const ToastContent& c);

// Windows' own XML parser (Windows.Data.Xml.Dom) takes it: what a toast is built from. Shows nothing.
bool ToastXmlLoads(const std::wstring& xml);

// ---- Registration (HKCU)

struct NotifyNames {
    std::wstring aumid;   // "Bytopsy.App"
    std::wstring scheme;  // "bytopsy"
};
// The real names; BYTOPSY_NOTIFY_TEST=1 gives "Bytopsy.Test" / "bytopsy-test" (self-tests).
NotifyNames DefaultNotifyNames();
std::wstring AumidKey(const NotifyNames& n);     // Software\Classes\AppUserModelId\<aumid>
std::wstring SchemeKey(const NotifyNames& n);    // Software\Classes\<scheme>
// Windows' own key for the app's notifications (the switch in Settings), created by Windows with
// the first notification: Software\Microsoft\Windows\CurrentVersion\Notifications\Settings\<aumid>
std::wstring SettingsKey(const NotifyNames& n);
// What the protocol runs: "<exe>" --alert-link "%1"
std::wstring LinkCommand(const std::wstring& exe);
// The icon shown on the toasts: a PNG made from the exe's icon, in util::DataDir().
std::wstring NotifyIconFile(const NotifyNames& n);

// Writes both keys (and the icon). `exe` is bytopsy.exe.
bool RegisterNotifications(const NotifyNames& n, const std::wstring& exe, std::wstring& err);
// Removes both keys, the icon and Bytopsy's notifications from the Action Center. A missing key is fine.
void UnregisterNotifications(const NotifyNames& n);
// Both keys are there.
bool NotificationsRegistered(const NotifyNames& n);
// The protocol's command, or "" when it isn't registered.
std::wstring RegisteredLinkCommand(const NotifyNames& n);

// ---- Showing

enum class ToastResult {
    Shown,
    TurnedOff,  // Windows Settings (or policy) turned off notifications for Bytopsy or for everything
    Failed,     // the notification API isn't available or refused: use the balloon
};
// `tag` + `group` identify the toast: a new one with the same pair replaces it, RemoveToast takes
// it out of the Action Center. `hours`: it expires after that many hours (0 = Windows' default).
ToastResult ShowToast(const NotifyNames& n, const ToastContent& c, const std::wstring& tag, const std::wstring& group, int hours,
                      std::wstring& err);
void RemoveToast(const NotifyNames& n, const std::wstring& tag, const std::wstring& group);
// Windows' setting for these notifications: 0 on, 1 off for Bytopsy, 2 off for this user,
// 3 off by policy, 4 off by the manifest, -1 unknown (the API isn't available).
int NotificationSetting(const NotifyNames& n);

// The fallback: a balloon from a notification-area icon, shown for up to `waitMs` (the call
// blocks that long at most). A click on it starts `exe args`. False if it couldn't be shown.
bool ShowBalloon(const ToastContent& c, const std::wstring& exe, const std::wstring& args, unsigned waitMs);
