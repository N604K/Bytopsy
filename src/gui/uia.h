#pragma once

// UI Automation provider for the main window: screen readers (Narrator, NVDA, JAWS) and other
// UIA clients see the elements of what is on screen, built every frame into an AccTree
// (accmodel.h), and can act on them through patterns that run the same code a click runs.

#include <windows.h>
#include <uiautomation.h>

#include <cstdint>
#include <memory>
#include <string>

#include "accmodel.h"

enum class AccAct { Invoke, Toggle, Select, AddToSelection, RemoveFromSelection, Expand, Collapse, ScrollIntoView, Focus };

// What the provider needs from the window. Every call arrives on the window's thread (the
// provider asks UIA for COM threading, and the window's thread is an STA).
class AccHost {
public:
    virtual HWND AccWindow() const = 0;
    // The tree as it is on screen now (paints first when a change hasn't been drawn yet); null when gone.
    virtual const AccTree* AccCurrent() = 0;
    virtual HRESULT AccDo(uint64_t key, AccAct act) = 0;
    virtual RECT AccScreenRect(const RectF& r) const = 0;
    virtual bool AccClientPoint(double screenX, double screenY, float* x, float* y) const = 0;

protected:
    ~AccHost() = default;
};

struct AccLink;

class AccBridge {
public:
    explicit AccBridge(AccHost* host);
    ~AccBridge();
    AccBridge(const AccBridge&) = delete;
    AccBridge& operator=(const AccBridge&) = delete;

    // WM_GETOBJECT: returns true (and the result) when it was a UI Automation request.
    bool OnGetObject(WPARAM wp, LPARAM lp, LRESULT* result);
    bool Active() const { return active_; }  // a UIA client has asked for the window at least once
    bool Listening() const;                  // some client listens for events
    void Detach();                           // WM_DESTROY: providers fail gracefully from now on

    // Events. key 0 is the window itself.
    void FocusChanged(uint64_t key);
    void StructureChanged();
    void WindowOpened(uint64_t key);
    void PropertyChanged(uint64_t key, PROPERTYID id, VARIANT oldValue, VARIANT newValue);
    void Notify(const std::wstring& text);  // spoken by screen readers (a toast)

private:
    IRawElementProviderSimple* Provider(uint64_t key);  // AddRef'ed
    std::shared_ptr<AccLink> link_;
    bool active_ = false;
};
