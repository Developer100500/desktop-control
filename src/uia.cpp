#include "control.hpp"
#include <UIAutomation.h>
namespace dc
{
namespace
{
winrt::com_ptr<IUIAutomation> automation;
std::map<std::string, winrt::com_ptr<IUIAutomationElement>> elements;
std::string snapshot;
HWND snapshot_window{};
std::string bstr_text(BSTR text)
{
    std::string out = text ? utf8(std::wstring(text, SysStringLen(text))) : "";
    SysFreeString(text);
    return out;
}
json bounds(IUIAutomationElement *e)
{
    RECT r{};
    if (FAILED(e->get_CurrentBoundingRectangle(&r)))
        return nullptr;
    return rect_json(r);
}
json read_tree(HWND hwnd, std::string const &id, unsigned timeout)
{
    // Read hwnd UIA tree for snapshot id; timeout bounds traversal in milliseconds.
    worker_event("reading_uia");
    snapshot = id;
    snapshot_window = hwnd;
    elements.clear();
    winrt::com_ptr<IUIAutomationElement> root;
    winrt::check_hresult(automation->ElementFromHandle(hwnd, root.put()));
    winrt::com_ptr<IUIAutomationTreeWalker> walker;
    winrt::check_hresult(automation->get_ControlViewWalker(walker.put()));
    struct Item
    {
        winrt::com_ptr<IUIAutomationElement> e;
        int depth;
        std::string parent;
    };
    std::deque<Item> queue;
    queue.push_back({root, 0, ""});
    json nodes = json::array();
    auto started = Clock::now();
    bool truncated = false;
    while (!queue.empty())
    {
        if (nodes.size() >= 2000 || Clock::now() - started > std::chrono::milliseconds(timeout))
        {
            truncated = true;
            break;
        }
        auto item = queue.front();
        queue.pop_front();
        DWORD self = GetCurrentProcessId();
        int pid{};
        item.e->get_CurrentProcessId(&pid);
        if (DWORD(pid) == self)
            continue;
        auto eid = "element-" + std::to_string(nodes.size());
        elements.emplace(eid, item.e);
        BSTR name{}, aid{};
        CONTROLTYPEID type{};
        BOOL enabled{}, focus{}, password{};
        item.e->get_CurrentName(&name);
        item.e->get_CurrentAutomationId(&aid);
        item.e->get_CurrentControlType(&type);
        item.e->get_CurrentIsEnabled(&enabled);
        item.e->get_CurrentHasKeyboardFocus(&focus);
        item.e->get_CurrentIsPassword(&password);
        json node = {{"element_id", eid},
                     {"parent_id", item.parent},
                     {"depth", item.depth},
                     {"name", bstr_text(name)},
                     {"automation_id", bstr_text(aid)},
                     {"control_type", type},
                     {"bounds", bounds(item.e.get())},
                     {"enabled", bool(enabled)},
                     {"focused", bool(focus)},
                     {"password", bool(password)}};
        json patterns = json::array();
        for (auto p :
             std::vector<std::pair<PATTERNID, std::string>>{{UIA_InvokePatternId, "invoke"},
                                                            {UIA_ValuePatternId, "value"},
                                                            {UIA_TogglePatternId, "toggle"},
                                                            {UIA_SelectionItemPatternId, "selection"},
                                                            {UIA_ExpandCollapsePatternId, "expand_collapse"},
                                                            {UIA_ScrollPatternId, "scroll"},
                                                            {UIA_TextPatternId, "text"}})
        {
            winrt::com_ptr<IUnknown> object;
            if (SUCCEEDED(item.e->GetCurrentPattern(p.first, object.put())) && object)
                patterns.push_back(p.second);
        }
        node["patterns"] = patterns;
        if (!password)
        {
            winrt::com_ptr<IUIAutomationValuePattern> value;
            if (SUCCEEDED(item.e->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(value.put()))) &&
                value)
            {
                BSTR text{};
                if (SUCCEEDED(value->get_CurrentValue(&text)))
                    node["value"] = bstr_text(text);
            }
            winrt::com_ptr<IUIAutomationTextPattern> text;
            if (SUCCEEDED(item.e->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(text.put()))) && text)
            {
                winrt::com_ptr<IUIAutomationTextRange> range;
                if (SUCCEEDED(text->get_DocumentRange(range.put())) && range)
                {
                    BSTR content{};
                    if (SUCCEEDED(range->GetText(16384, &content)))
                        node["text"] = bstr_text(content);
                }
            }
        }
        nodes.push_back(node);
        if (item.depth >= 30)
        {
            winrt::com_ptr<IUIAutomationElement> child;
            if (SUCCEEDED(walker->GetFirstChildElement(item.e.get(), child.put())) && child)
                truncated = true;
            continue;
        }
        winrt::com_ptr<IUIAutomationElement> child;
        if (SUCCEEDED(walker->GetFirstChildElement(item.e.get(), child.put())))
        {
            while (child)
            {
                if (queue.size() + nodes.size() >= 2000 || Clock::now() - started > std::chrono::milliseconds(timeout))
                {
                    truncated = true;
                    break;
                }
                queue.push_back({child, item.depth + 1, eid});
                winrt::com_ptr<IUIAutomationElement> sibling;
                if (FAILED(walker->GetNextSiblingElement(child.get(), sibling.put())))
                    break;
                child = std::move(sibling);
            }
        }
    }
    return {{"observation_id", id}, {"elements", nodes}, {"truncated", truncated}};
}
template <class T> winrt::com_ptr<T> pattern(IUIAutomationElement *e, PATTERNID id)
{
    winrt::com_ptr<T> p;
    winrt::check_hresult(e->GetCurrentPatternAs(id, __uuidof(T), p.put_void()));
    if (!p)
        throw Error("PATTERN_UNSUPPORTED", "This element does not implement the requested pattern");
    return p;
}
json act(json const &req)
{
    if (req.at("observation_id").get<std::string>() != snapshot ||
        reinterpret_cast<HWND>(req.at("hwnd").get<uint64_t>()) != snapshot_window)
        throw Error("STALE_ELEMENT", "UIA observation is no longer available");
    auto found = elements.find(req.at("element_id").get<std::string>());
    if (found == elements.end())
        throw Error("STALE_ELEMENT", "Unknown UIA element");
    auto e = found->second;
    BOOL enabled{};
    winrt::check_hresult(e->get_CurrentIsEnabled(&enabled));
    if (!enabled)
        throw Error("ELEMENT_DISABLED", "UIA element is disabled");
    winrt::com_ptr<IUIAutomationElement> root;
    winrt::check_hresult(automation->ElementFromHandle(snapshot_window, root.put()));
    winrt::com_ptr<IUIAutomationTreeWalker> walker;
    winrt::check_hresult(automation->get_RawViewWalker(walker.put()));
    if (!walker || !root)
        throw Error("UIA_UNAVAILABLE", "UIA returned no root or tree walker");
    auto cursor = e;
    bool belongs = false;
    for (int i = 0; i < 128 && cursor; ++i)
    {
        BOOL same{};
        automation->CompareElements(cursor.get(), root.get(), &same);
        if (same)
        {
            belongs = true;
            break;
        }
        winrt::com_ptr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(cursor.get(), parent.put())))
            break;
        cursor = std::move(parent);
    }
    if (!belongs)
        throw Error("STALE_ELEMENT", "Element no longer belongs to target window");
    auto action = req.at("action").get<std::string>();
    worker_event("element_action");
    // Once a provider operation is entered, an exception cannot prove no side effect occurred.
    try
    {
        if (action == "invoke")
            winrt::check_hresult(pattern<IUIAutomationInvokePattern>(e.get(), UIA_InvokePatternId)->Invoke());
        else if (action == "set_value")
        {
            BOOL password{};
            e->get_CurrentIsPassword(&password);
            if (password)
                throw Error("PASSWORD_ELEMENT", "Password controls are excluded");
            auto p = pattern<IUIAutomationValuePattern>(e.get(), UIA_ValuePatternId);
            BOOL ro{};
            winrt::check_hresult(p->get_CurrentIsReadOnly(&ro));
            if (ro)
                throw Error("READ_ONLY", "Element is read-only");
            auto text = wide(req.at("value").get<std::string>());
            BSTR value = SysAllocStringLen(text.data(), (UINT)text.size());
            if (!value)
                throw std::bad_alloc();
            HRESULT hr = p->SetValue(value);
            SysFreeString(value);
            winrt::check_hresult(hr);
        }
        else if (action == "toggle")
            winrt::check_hresult(pattern<IUIAutomationTogglePattern>(e.get(), UIA_TogglePatternId)->Toggle());
        else if (action == "select")
            winrt::check_hresult(
                pattern<IUIAutomationSelectionItemPattern>(e.get(), UIA_SelectionItemPatternId)->Select());
        else if (action == "expand")
            winrt::check_hresult(
                pattern<IUIAutomationExpandCollapsePattern>(e.get(), UIA_ExpandCollapsePatternId)->Expand());
        else if (action == "collapse")
            winrt::check_hresult(
                pattern<IUIAutomationExpandCollapsePattern>(e.get(), UIA_ExpandCollapsePatternId)
                    ->Collapse());
        else if (action == "scroll")
            winrt::check_hresult(pattern<IUIAutomationScrollPattern>(e.get(), UIA_ScrollPatternId)
                                     ->Scroll(static_cast<ScrollAmount>(req.value("horizontal", 2)),
                                              static_cast<ScrollAmount>(req.value("vertical", 2))));
        else
            throw Error("INVALID_ACTION", "Unsupported element action");
    }
    catch (winrt::hresult_error const &err)
    {
        throw Error("UIA_ACTION_FAILED", winrt::to_string(err.message()), "element_action", err.code().value,
                    true);
    }
    elements.clear();
    snapshot.clear();
    return {{"outcome", "delivered"}};
}
} // namespace
json uia_request(json const &req)
{
    // Dispatch UIA; req supplies target, operation and optional traversal timeout.
    if (!automation)
        winrt::check_hresult(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(automation.put())));
    auto hwnd = reinterpret_cast<HWND>(req.at("hwnd").get<uint64_t>());
    if (!IsWindow(hwnd))
        throw Error("WINDOW_GONE", "UIA target no longer exists");
    if (req.at("operation") == "observe")
        return read_tree(hwnd, req.at("observation_id"), req.value("timeout_ms", unsigned(3000)));
    if (req.at("operation") == "action")
        return act(req);
    throw Error("INVALID_OPERATION", "Unknown UIA worker operation");
}
} // namespace dc
