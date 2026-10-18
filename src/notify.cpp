#include "notify.h"

#include <windows.h>
#include <roapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <windows.data.xml.dom.h>
#include <windows.foundation.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include "util.h"

#pragma comment(lib, "runtimeobject.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wun = ABI::Windows::UI::Notifications;
namespace wxml = ABI::Windows::Data::Xml::Dom;
namespace wf = ABI::Windows::Foundation;

// =============================================================================================
// XML

std::wstring EscapeXml(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 16);
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        switch (c) {
            case L'&': out += L"&amp;"; continue;
            case L'<': out += L"&lt;"; continue;
            case L'>': out += L"&gt;"; continue;
            case L'"': out += L"&quot;"; continue;
            case L'\'': out += L"&apos;"; continue;
            default: break;
        }
        if (c < 0x20 && c != L'\t' && c != L'\n' && c != L'\r') continue;  // not allowed in XML 1.0
        if (c == 0xFFFE || c == 0xFFFF) continue;
        if (c >= 0xD800 && c <= 0xDBFF) {  // a surrogate pair stays, a lone half goes
            if (i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                out += c;
                out += s[++i];
            }
            continue;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) continue;
        out += c;
    }
    return out;
}

std::wstring BuildToastXml(const ToastContent& c) {
    std::wstring x = L"<toast";
    if (!c.link.empty()) x += L" launch=\"" + EscapeXml(c.link) + L"\" activationType=\"protocol\"";
    x += L"><visual><binding template=\"ToastGeneric\">";
    x += L"<text>" + EscapeXml(c.title) + L"</text>";
    if (!c.body.empty()) x += L"<text>" + EscapeXml(c.body) + L"</text>";
    if (!c.footer.empty()) x += L"<text placement=\"attribution\">" + EscapeXml(c.footer) + L"</text>";
    x += L"</binding></visual>";
    if (!c.actions.empty()) {
        x += L"<actions>";
        for (auto& a : c.actions)
            x += L"<action content=\"" + EscapeXml(a.label) + L"\" activationType=\"protocol\" arguments=\"" + EscapeXml(a.link) + L"\"/>";
        x += L"</actions>";
    }
    x += L"</toast>";
    return x;
}

// =============================================================================================
// Registration

namespace {

bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool KeyExists(const std::wstring& key) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

// Deletes a key that holds nothing (no values, no subkeys): the AppUserModelId folder when
// Bytopsy was the only one in it.
void DeleteIfEmpty(const std::wstring& key) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return;
    DWORD subkeys = 1, values = 1;
    LONG r = RegQueryInfoKeyW(h, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(h);
    if (r == ERROR_SUCCESS && subkeys == 0 && values == 0) RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str());
}

bool IsTestMode() {
    wchar_t v[8];
    DWORD n = GetEnvironmentVariableW(L"BYTOPSY_NOTIFY_TEST", v, 8);
    return n && n < 8 && v[0] == L'1';
}

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
};

struct RoInit {
    HRESULT hr;
    RoInit() : hr(RoInitialize(RO_INIT_MULTITHREADED)) {}
    ~RoInit() {
        if (SUCCEEDED(hr)) RoUninitialize();
    }
    // Already initialized in another mode (the windowed app's UI thread) is fine too.
    bool Ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    RoInit(const RoInit&) = delete;
    RoInit& operator=(const RoInit&) = delete;
};

// The exe's own icon as a 64 x 64 PNG.
bool WriteIconPng(const std::wstring& exe, const std::wstring& png) {
    HICON icon = nullptr;
    if (FAILED(SHDefExtractIconW(exe.c_str(), 0, 0, &icon, nullptr, MAKELONG(64, 16))) || !icon) return false;
    ComInit com;
    bool ok = false;
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    std::wstring tmp = png + L".tmp";
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateBitmapFromHICON(icon, &bmp)) && SUCCEEDED(f->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
        UINT w = 0, h = 0;
        bmp->GetSize(&w, &h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WriteSource(bmp.Get(), nullptr)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    frame.Reset();
    enc.Reset();
    stream.Reset();  // closes the file
    DestroyIcon(icon);
    if (ok) ok = MoveFileExW(tmp.c_str(), png.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

}  // namespace

NotifyNames DefaultNotifyNames() {
    if (IsTestMode()) return {L"Bytopsy.Test", L"bytopsy-test"};
    return {L"Bytopsy.App", L"bytopsy"};
}

std::wstring AumidKey(const NotifyNames& n) { return L"Software\\Classes\\AppUserModelId\\" + n.aumid; }
std::wstring SchemeKey(const NotifyNames& n) { return L"Software\\Classes\\" + n.scheme; }
std::wstring SettingsKey(const NotifyNames& n) { return L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings\\" + n.aumid; }
std::wstring LinkCommand(const std::wstring& exe) { return L"\"" + exe + L"\" --alert-link \"%1\""; }

std::wstring NotifyIconFile(const NotifyNames& n) {
    std::wstring dir = util::DataDir();
    return dir.empty() ? L"" : dir + L"\\" + (n.aumid == L"Bytopsy.App" ? std::wstring(L"notify-icon.png") : n.aumid + L"-icon.png");
}

bool RegisterNotifications(const NotifyNames& n, const std::wstring& exe, std::wstring& err) {
    std::wstring icon = NotifyIconFile(n);
    if (!icon.empty()) {
        CreateDirectoryW(util::DataDir().c_str(), nullptr);
        if (!WriteIconPng(exe, icon)) icon.clear();  // the toast then shows a plain icon
    }
    std::wstring a = AumidKey(n), s = SchemeKey(n);
    bool ok = SetString(a, L"DisplayName", L"Bytopsy") && SetString(a, L"IconUri", icon) && SetString(s, nullptr, L"URL:Bytopsy link") &&
              SetString(s, L"URL Protocol", L"") && SetString(s + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0") &&
              SetString(s + L"\\shell\\open\\command", nullptr, LinkCommand(exe));
    if (!ok) {
        err = L"Could not write to the registry";
        UnregisterNotifications(n);
    }
    return ok;
}

void UnregisterNotifications(const NotifyNames& n) {
    {
        RoInit ro;
        ComPtr<wun::IToastNotificationManagerStatics2> mgr;
        ComPtr<wun::IToastNotificationHistory> hist;
        if (ro.Ok() &&
            SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&mgr))) &&
            SUCCEEDED(mgr->get_History(&hist)))
            hist->ClearWithId(HStringReference(n.aumid.c_str()).Get());
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, AumidKey(n).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, AumidKey(n).c_str());
    DeleteIfEmpty(L"Software\\Classes\\AppUserModelId");
    RegDeleteTreeW(HKEY_CURRENT_USER, SchemeKey(n).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, SchemeKey(n).c_str());
    // Windows' own record of the app (written when its first notification is shown; it holds the
    // on/off switch in Settings) goes too, so nothing of Bytopsy is left behind.
    RegDeleteTreeW(HKEY_CURRENT_USER, SettingsKey(n).c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, SettingsKey(n).c_str());
    std::wstring icon = NotifyIconFile(n);
    if (!icon.empty()) DeleteFileW(icon.c_str());
}

bool NotificationsRegistered(const NotifyNames& n) { return KeyExists(AumidKey(n)) && KeyExists(SchemeKey(n)); }

std::wstring RegisteredLinkCommand(const NotifyNames& n) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, (SchemeKey(n) + L"\\shell\\open\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return L"";
    return buf;
}

// =============================================================================================
// Toasts

namespace {

bool Notifier(const NotifyNames& n, ComPtr<wun::IToastNotifier>& notifier, ComPtr<wun::IToastNotificationManagerStatics>* mgrOut, std::wstring& err) {
    ComPtr<wun::IToastNotificationManagerStatics> mgr;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&mgr));
    if (SUCCEEDED(hr)) hr = mgr->CreateToastNotifierWithId(HStringReference(n.aumid.c_str()).Get(), &notifier);
    if (FAILED(hr)) {
        err = util::Win32ErrorMessage(static_cast<unsigned long>(hr));
        return false;
    }
    if (mgrOut) *mgrOut = mgr;
    return true;
}

}  // namespace

bool ToastXmlLoads(const std::wstring& xml) {
    RoInit ro;
    if (!ro.Ok()) return false;
    ComPtr<IInspectable> insp;
    ComPtr<wxml::IXmlDocument> doc;
    ComPtr<wxml::IXmlDocumentIO> io;
    return SUCCEEDED(RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &insp)) && SUCCEEDED(insp.As(&doc)) &&
           SUCCEEDED(doc.As(&io)) && SUCCEEDED(io->LoadXml(HStringReference(xml.c_str()).Get()));
}

namespace {

// What the switches in Settings, System, Notifications store: all notifications
// (PushNotifications ToastEnabled) and the app's own (Notifications\Settings\<id> Enabled).
// ToastNotifier.Setting doesn't answer for apps without a package.
int SettingFromRegistry(const NotifyNames& n) {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications", L"ToastEnabled", RRF_RT_REG_DWORD,
                     nullptr, &v, &size) == ERROR_SUCCESS &&
        v == 0)
        return 2;
    v = 1;
    size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, SettingsKey(n).c_str(), L"Enabled", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v == 0) return 1;
    return 0;
}

}  // namespace

int NotificationSetting(const NotifyNames& n) {
    {
        RoInit ro;
        ComPtr<wun::IToastNotifier> notifier;
        std::wstring err;
        wun::NotificationSetting s;
        if (ro.Ok() && Notifier(n, notifier, nullptr, err) && SUCCEEDED(notifier->get_Setting(&s))) return static_cast<int>(s);
    }
    return SettingFromRegistry(n);
}

ToastResult ShowToast(const NotifyNames& n, const ToastContent& c, const std::wstring& tag, const std::wstring& group, int hours,
                      std::wstring& err) {
    RoInit ro;
    if (!ro.Ok()) {
        err = util::Win32ErrorMessage(static_cast<unsigned long>(ro.hr));
        return ToastResult::Failed;
    }
    ComPtr<wun::IToastNotifier> notifier;
    if (!Notifier(n, notifier, nullptr, err)) return ToastResult::Failed;
    wun::NotificationSetting setting = wun::NotificationSetting_Enabled;
    if (FAILED(notifier->get_Setting(&setting))) setting = static_cast<wun::NotificationSetting>(SettingFromRegistry(n));
    if (setting != wun::NotificationSetting_Enabled) {
        err = L"notifications are turned off";
        return ToastResult::TurnedOff;
    }
    ComPtr<IInspectable> insp;
    ComPtr<wxml::IXmlDocument> doc;
    ComPtr<wxml::IXmlDocumentIO> io;
    std::wstring xml = BuildToastXml(c);
    HRESULT hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &insp);
    if (SUCCEEDED(hr)) hr = insp.As(&doc);
    if (SUCCEEDED(hr)) hr = doc.As(&io);
    if (SUCCEEDED(hr)) hr = io->LoadXml(HStringReference(xml.c_str()).Get());
    ComPtr<wun::IToastNotificationFactory> factory;
    ComPtr<wun::IToastNotification> toast;
    if (SUCCEEDED(hr))
        hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(), IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateToastNotification(doc.Get(), &toast);
    if (FAILED(hr)) {
        err = util::Win32ErrorMessage(static_cast<unsigned long>(hr));
        return ToastResult::Failed;
    }
    ComPtr<wun::IToastNotification2> t2;
    if (SUCCEEDED(toast.As(&t2))) {
        if (!tag.empty()) t2->put_Tag(HStringReference(tag.c_str()).Get());
        if (!group.empty()) t2->put_Group(HStringReference(group.c_str()).Get());
    }
    if (hours > 0) {
        ComPtr<wf::IPropertyValueStatics> pv;
        ComPtr<IInspectable> boxed;
        ComPtr<wf::IReference<wf::DateTime>> ref;
        wf::DateTime when{util::NowFileTime() + static_cast<int64_t>(hours) * 3600 * 10000000LL};
        if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_PropertyValue).Get(), IID_PPV_ARGS(&pv))) &&
            SUCCEEDED(pv->CreateDateTime(when, &boxed)) && SUCCEEDED(boxed.As(&ref)))
            toast->put_ExpirationTime(ref.Get());
    }
    hr = notifier->Show(toast.Get());
    if (FAILED(hr)) {
        err = util::Win32ErrorMessage(static_cast<unsigned long>(hr));
        return ToastResult::Failed;
    }
    return ToastResult::Shown;
}

void RemoveToast(const NotifyNames& n, const std::wstring& tag, const std::wstring& group) {
    RoInit ro;
    if (!ro.Ok()) return;
    ComPtr<wun::IToastNotificationManagerStatics2> mgr;
    ComPtr<wun::IToastNotificationHistory> hist;
    if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&mgr))) &&
        SUCCEEDED(mgr->get_History(&hist)))
        hist->RemoveGroupedTagWithId(HStringReference(tag.c_str()).Get(), HStringReference(group.c_str()).Get(),
                                     HStringReference(n.aumid.c_str()).Get());
}

// =============================================================================================
// Balloon (fallback)

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;

struct BalloonState {
    std::wstring exe, args;
    bool clicked = false;
};

LRESULT CALLBACK BalloonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<BalloonState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == kTrayMsg && st) {
        UINT ev = LOWORD(lp);
        if (ev == NIN_BALLOONUSERCLICK || ev == WM_LBUTTONUP) {
            st->clicked = true;
            ShellExecuteW(nullptr, L"open", st->exe.c_str(), st->args.c_str(), nullptr, SW_SHOWNORMAL);
            PostQuitMessage(0);
        } else if (ev == NIN_BALLOONTIMEOUT) {
            PostQuitMessage(0);
        }
        return 0;
    }
    if (msg == WM_TIMER) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool ShowBalloon(const ToastContent& c, const std::wstring& exe, const std::wstring& args, unsigned waitMs) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = BalloonProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"BytopsyAlertBalloon";
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Bytopsy", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!h) return false;
    BalloonState st{exe, args};
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = h;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_INFO | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    HICON own = nid.hIcon;
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_WARNING);
    wcsncpy_s(nid.szTip, L"Bytopsy", _TRUNCATE);
    wcsncpy_s(nid.szInfoTitle, c.title.c_str(), _TRUNCATE);
    std::wstring body = c.body + (c.footer.empty() ? L"" : L"\n" + c.footer);
    wcsncpy_s(nid.szInfo, body.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING | NIIF_RESPECT_QUIET_TIME;
    bool ok = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (ok) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        SetTimer(h, 1, waitMs, nullptr);
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    DestroyWindow(h);
    if (own) DestroyIcon(own);
    return ok;
}
