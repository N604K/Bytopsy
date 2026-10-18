#include "uia.h"

#include <atomic>
#include <vector>

#pragma comment(lib, "uiautomationcore.lib")

// Shared by the bridge and every provider object it hands out: a client may keep an element long
// after the window is gone, so providers reach the window only through this, and fail with
// UIA_E_ELEMENTNOTAVAILABLE once it is cleared.
struct AccLink {
    AccHost* host = nullptr;
    IRawElementProviderSimple* root = nullptr;
};

namespace {

BSTR Bstr(const std::wstring& s) { return SysAllocStringLen(s.c_str(), static_cast<UINT>(s.size())); }

class Element final : public IRawElementProviderSimple,
                      public IRawElementProviderFragment,
                      public IRawElementProviderFragmentRoot,
                      public IInvokeProvider,
                      public IToggleProvider,
                      public ISelectionItemProvider,
                      public ISelectionProvider,
                      public IExpandCollapseProvider,
                      public IScrollItemProvider {
public:
    // key 0: the window itself (the fragment root, hosted by the HWND).
    Element(std::shared_ptr<AccLink> link, uint64_t key) : link_(std::move(link)), key_(key) {}

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
            *out = static_cast<IRawElementProviderSimple*>(this);
        else if (riid == __uuidof(IRawElementProviderFragment))
            *out = static_cast<IRawElementProviderFragment*>(this);
        else if (riid == __uuidof(IRawElementProviderFragmentRoot) && !key_)
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (key_ && riid == __uuidof(IInvokeProvider))
            *out = static_cast<IInvokeProvider*>(this);
        else if (key_ && riid == __uuidof(IToggleProvider))
            *out = static_cast<IToggleProvider*>(this);
        else if (key_ && riid == __uuidof(ISelectionItemProvider))
            *out = static_cast<ISelectionItemProvider*>(this);
        else if (key_ && riid == __uuidof(ISelectionProvider))
            *out = static_cast<ISelectionProvider*>(this);
        else if (key_ && riid == __uuidof(IExpandCollapseProvider))
            *out = static_cast<IExpandCollapseProvider*>(this);
        else if (key_ && riid == __uuidof(IScrollItemProvider))
            *out = static_cast<IScrollItemProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override {
        if (!out) return E_POINTER;
        *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID id, IUnknown** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!key_) return S_OK;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        if (!AccSupports(*n, id)) return S_OK;
        switch (id) {
            case UIA_InvokePatternId: *out = static_cast<IInvokeProvider*>(this); break;
            case UIA_TogglePatternId: *out = static_cast<IToggleProvider*>(this); break;
            case UIA_SelectionItemPatternId: *out = static_cast<ISelectionItemProvider*>(this); break;
            case UIA_SelectionPatternId: *out = static_cast<ISelectionProvider*>(this); break;
            case UIA_ExpandCollapsePatternId: *out = static_cast<IExpandCollapseProvider*>(this); break;
            case UIA_ScrollItemPatternId: *out = static_cast<IScrollItemProvider*>(this); break;
            default: return S_OK;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* v) override {
        if (!v) return E_POINTER;
        VariantInit(v);
        if (!key_) {
            // The window: the HWND's own provider gives name, type and the rest.
            if (id == UIA_FrameworkIdPropertyId) {
                v->vt = VT_BSTR;
                v->bstrVal = SysAllocString(L"Bytopsy");
            }
            return S_OK;
        }
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        auto boolean = [&](bool b) {
            v->vt = VT_BOOL;
            v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
        };
        auto str = [&](const std::wstring& s) {
            v->vt = VT_BSTR;
            v->bstrVal = Bstr(s);
        };
        switch (id) {
            case UIA_ControlTypePropertyId:
                v->vt = VT_I4;
                v->lVal = AccControlType(n->role);
                break;
            case UIA_NamePropertyId: str(n->name); break;
            case UIA_HelpTextPropertyId:
                if (!n->help.empty()) str(n->help);
                break;
            case UIA_FullDescriptionPropertyId:
                if (!n->help.empty()) str(n->help);
                break;
            case UIA_AutomationIdPropertyId:
                if (!n->id.empty()) str(n->id);
                break;
            case UIA_FrameworkIdPropertyId: str(L"Bytopsy"); break;
            case UIA_IsEnabledPropertyId: boolean(!(n->flags & AF_DISABLED)); break;
            case UIA_IsKeyboardFocusablePropertyId: boolean((n->flags & AF_FOCUSABLE) != 0); break;
            case UIA_HasKeyboardFocusPropertyId: boolean((n->flags & AF_FOCUSED) != 0); break;
            case UIA_IsControlElementPropertyId:
            case UIA_IsContentElementPropertyId: boolean(true); break;
            case UIA_IsOffscreenPropertyId: boolean(false); break;
            case UIA_IsDialogPropertyId: boolean((n->flags & AF_MODAL) != 0); break;
            case UIA_LevelPropertyId:
                if (n->level > 0) {
                    v->vt = VT_I4;
                    v->lVal = n->level;
                }
                break;
            case UIA_LandmarkTypePropertyId:
                if (n->flags & (AF_MAIN | AF_NAV)) {
                    v->vt = VT_I4;
                    v->lVal = (n->flags & AF_NAV) ? UIA_NavigationLandmarkTypeId : UIA_MainLandmarkTypeId;
                }
                break;
            default: break;
        }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (key_) return S_OK;
        AccHost* h = link_->host;
        if (!h) return UIA_E_ELEMENTNOTAVAILABLE;
        return UiaHostProviderFromHwnd(h->AccWindow(), out);
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection dir, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        AccHost* h = link_->host;
        const AccTree* t = h ? h->AccCurrent() : nullptr;
        if (!t) return UIA_E_ELEMENTNOTAVAILABLE;
        int i = -1;
        if (key_) {
            i = t->Find(key_);
            if (i < 0) return UIA_E_ELEMENTNOTAVAILABLE;
        }
        int to = -1;
        bool toRoot = false;
        switch (dir) {
            case NavigateDirection_Parent:
                if (!key_) return S_OK;
                to = t->Parent(i);
                toRoot = to < 0;
                break;
            case NavigateDirection_NextSibling:
                if (!key_) return S_OK;
                to = t->Next(i);
                break;
            case NavigateDirection_PreviousSibling:
                if (!key_) return S_OK;
                to = t->Prev(i);
                break;
            case NavigateDirection_FirstChild: to = t->FirstChild(i); break;
            case NavigateDirection_LastChild: to = t->LastChild(i); break;
            default: return S_OK;
        }
        if (toRoot) return link_->root ? link_->root->QueryInterface(IID_PPV_ARGS(out)) : S_OK;
        if (to < 0) return S_OK;
        *out = new Element(link_, (*t)[to].key);
        return S_OK;
    }
    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!key_) return S_OK;  // the HWND's provider supplies the window's
        HRESULT hr;
        if (!Node(&hr)) return hr;
        int ids[3] = {UiaAppendRuntimeId, static_cast<int>(key_ & 0xFFFFFFFF), static_cast<int>(key_ >> 32)};
        SAFEARRAY* sa = SafeArrayCreateVector(VT_I4, 0, 3);
        if (!sa) return E_OUTOFMEMORY;
        for (LONG k = 0; k < 3; ++k) SafeArrayPutElement(sa, &k, &ids[k]);
        *out = sa;
        return S_OK;
    }
    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
        if (!out) return E_POINTER;
        *out = UiaRect{};
        if (!key_) return S_OK;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        RECT r = link_->host->AccScreenRect(n->r);
        *out = UiaRect{static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right - r.left),
                       static_cast<double>(r.bottom - r.top)};
        return S_OK;
    }
    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override { return key_ ? Do(AccAct::Focus) : S_OK; }
    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!link_->root) return UIA_E_ELEMENTNOTAVAILABLE;
        return link_->root->QueryInterface(IID_PPV_ARGS(out));
    }

    // IRawElementProviderFragmentRoot (the window only)
    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        AccHost* h = link_->host;
        const AccTree* t = h ? h->AccCurrent() : nullptr;
        if (!t) return UIA_E_ELEMENTNOTAVAILABLE;
        float cx, cy;
        if (!h->AccClientPoint(x, y, &cx, &cy)) return S_OK;
        int i = t->FromPoint(cx, cy);
        if (i >= 0) *out = new Element(link_, (*t)[i].key);
        return S_OK;
    }
    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        AccHost* h = link_->host;
        const AccTree* t = h ? h->AccCurrent() : nullptr;
        if (!t) return UIA_E_ELEMENTNOTAVAILABLE;
        int i = t->Focused();
        if (i >= 0) *out = new Element(link_, (*t)[i].key);
        return S_OK;
    }

    // Patterns
    IFACEMETHODIMP Invoke() override { return Do(AccAct::Invoke); }
    IFACEMETHODIMP Toggle() override { return Do(AccAct::Toggle); }
    IFACEMETHODIMP get_ToggleState(ToggleState* out) override {
        if (!out) return E_POINTER;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        *out = (n->flags & AF_MIXED) ? ToggleState_Indeterminate : (n->flags & AF_ON) ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }
    IFACEMETHODIMP Select() override { return Do(AccAct::Select); }
    IFACEMETHODIMP AddToSelection() override { return Do(AccAct::AddToSelection); }
    IFACEMETHODIMP RemoveFromSelection() override { return Do(AccAct::RemoveFromSelection); }
    IFACEMETHODIMP get_IsSelected(BOOL* out) override {
        if (!out) return E_POINTER;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        *out = (n->flags & AF_SELECTED) ? TRUE : FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        const AccTree* t = link_->host->AccCurrent();
        for (int p = n->parent; p >= 0; p = t->Parent(p))
            if ((*t)[p].flags & AF_SELECTION) {
                *out = new Element(link_, (*t)[p].key);
                break;
            }
        return S_OK;
    }
    IFACEMETHODIMP GetSelection(SAFEARRAY** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        HRESULT hr;
        if (!Node(&hr)) return hr;
        const AccTree* t = link_->host->AccCurrent();
        int me = t->Find(key_);
        std::vector<uint64_t> keys;
        // Selected items anywhere below (the list's rows may sit under a group).
        std::vector<int> stack((*t)[me].kids.begin(), (*t)[me].kids.end());
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            if ((*t)[i].flags & AF_SELECTED) keys.push_back((*t)[i].key);
            if (!((*t)[i].flags & AF_SELECTION)) stack.insert(stack.end(), (*t)[i].kids.begin(), (*t)[i].kids.end());
        }
        SAFEARRAY* sa = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(keys.size()));
        if (!sa) return E_OUTOFMEMORY;
        for (LONG k = 0; k < static_cast<LONG>(keys.size()); ++k) {
            IRawElementProviderSimple* e = new Element(link_, keys[static_cast<size_t>(k)]);
            SafeArrayPutElement(sa, &k, e);  // adds its own reference
            e->Release();
        }
        *out = sa;
        return S_OK;
    }
    IFACEMETHODIMP get_CanSelectMultiple(BOOL* out) override {
        if (!out) return E_POINTER;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        *out = (n->flags & AF_MULTI) ? TRUE : FALSE;
        return S_OK;
    }
    IFACEMETHODIMP get_IsSelectionRequired(BOOL* out) override {
        if (!out) return E_POINTER;
        *out = FALSE;
        return S_OK;
    }
    IFACEMETHODIMP Expand() override { return Do(AccAct::Expand); }
    IFACEMETHODIMP Collapse() override { return Do(AccAct::Collapse); }
    IFACEMETHODIMP get_ExpandCollapseState(ExpandCollapseState* out) override {
        if (!out) return E_POINTER;
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        *out = !(n->flags & AF_EXPANDABLE) ? ExpandCollapseState_LeafNode
               : (n->flags & AF_EXPANDED)  ? ExpandCollapseState_Expanded
                                           : ExpandCollapseState_Collapsed;
        return S_OK;
    }
    IFACEMETHODIMP ScrollIntoView() override { return Do(AccAct::ScrollIntoView); }

private:
    ~Element() = default;

    const AccNode* Node(HRESULT* hr) {
        AccHost* h = link_->host;
        const AccTree* t = h ? h->AccCurrent() : nullptr;
        int i = t ? t->Find(key_) : -1;
        if (i < 0) {
            *hr = UIA_E_ELEMENTNOTAVAILABLE;
            return nullptr;
        }
        *hr = S_OK;
        return &(*t)[i];
    }
    HRESULT Do(AccAct act) {
        HRESULT hr;
        const AccNode* n = Node(&hr);
        if (!n) return hr;
        if (n->flags & AF_DISABLED) return UIA_E_ELEMENTNOTENABLED;
        return link_->host->AccDo(key_, act);
    }

    std::atomic<ULONG> refs_{1};
    std::shared_ptr<AccLink> link_;
    uint64_t key_;
};

}  // namespace

AccBridge::AccBridge(AccHost* host) : link_(std::make_shared<AccLink>()) {
    link_->host = host;
    link_->root = new Element(link_, 0);
}

AccBridge::~AccBridge() { Detach(); }

bool AccBridge::OnGetObject(WPARAM wp, LPARAM lp, LRESULT* result) {
    if (static_cast<LONG>(lp) != static_cast<LONG>(UiaRootObjectId) || !link_->root || !link_->host) return false;
    active_ = true;
    *result = UiaReturnRawElementProvider(link_->host->AccWindow(), wp, lp, link_->root);
    return true;
}

bool AccBridge::Listening() const { return active_ && link_->root && UiaClientsAreListening(); }

void AccBridge::Detach() {
    if (!link_->root) return;
    if (link_->host) UiaReturnRawElementProvider(link_->host->AccWindow(), 0, 0, nullptr);
    UiaDisconnectProvider(link_->root);
    UiaDisconnectAllProviders();
    link_->host = nullptr;
    IRawElementProviderSimple* r = link_->root;
    link_->root = nullptr;
    r->Release();  // the root and every element share link_; it outlives them all
}

IRawElementProviderSimple* AccBridge::Provider(uint64_t key) {
    if (!link_->root) return nullptr;
    if (!key) {
        link_->root->AddRef();
        return link_->root;
    }
    return new Element(link_, key);
}

void AccBridge::FocusChanged(uint64_t key) {
    if (!Listening()) return;
    if (IRawElementProviderSimple* p = Provider(key)) {
        UiaRaiseAutomationEvent(p, UIA_AutomationFocusChangedEventId);
        p->Release();
    }
}

void AccBridge::StructureChanged() {
    if (!Listening()) return;
    if (IRawElementProviderSimple* p = Provider(0)) {
        UiaRaiseStructureChangedEvent(p, StructureChangeType_ChildrenInvalidated, nullptr, 0);
        p->Release();
    }
}

void AccBridge::WindowOpened(uint64_t key) {
    if (!Listening()) return;
    if (IRawElementProviderSimple* p = Provider(key)) {
        UiaRaiseAutomationEvent(p, UIA_Window_WindowOpenedEventId);
        p->Release();
    }
}

void AccBridge::PropertyChanged(uint64_t key, PROPERTYID id, VARIANT oldValue, VARIANT newValue) {
    if (!Listening()) return;
    if (IRawElementProviderSimple* p = Provider(key)) {
        UiaRaiseAutomationPropertyChangedEvent(p, id, oldValue, newValue);
        p->Release();
    }
}

void AccBridge::Notify(const std::wstring& text) {
    if (!Listening() || text.empty()) return;
    // Windows 10 1709 and later; looked up so the program still starts on older systems.
    using Fn = HRESULT(WINAPI*)(IRawElementProviderSimple*, NotificationKind, NotificationProcessing, BSTR, BSTR);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"uiautomationcore.dll"), "UiaRaiseNotificationEvent"));
    if (!fn) return;
    if (IRawElementProviderSimple* p = Provider(0)) {
        BSTR t = Bstr(text), id = SysAllocString(L"Bytopsy.Message");
        fn(p, NotificationKind_Other, NotificationProcessing_ImportantMostRecent, t, id);
        SysFreeString(t);
        SysFreeString(id);
        p->Release();
    }
}
