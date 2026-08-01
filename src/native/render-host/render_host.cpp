// Taskbar Widgets web renderer. This process is intentionally separate from
// explorer.exe: package HTML/JS is never loaded by the taskbar hook.
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <d3d11.h>
#include <dcomp.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <sddl.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"TaskbarWidgets.RenderHost.Overlay";
constexpr UINT kPipeMessage = WM_APP + 42;
constexpr UINT_PTR kSnapshotTimer = 7;
constexpr UINT_PTR kVisibilityTimer = 8;
constexpr DWORD kMaximumPipeMessage = 256 * 1024;

std::wstring g_dataDirectory;
std::wstring g_communityDirectory;
std::wstring g_storageDirectory;
ComPtr<ICoreWebView2Environment> g_environment;
std::mutex g_pipeMutex;
std::vector<std::wstring> g_pipeMessages;
std::atomic_bool g_stopping = false;

std::wstring ReadArgument(int argc, wchar_t** argv, const wchar_t* name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (_wcsicmp(argv[index], name) == 0) return argv[index + 1];
    }
    return {};
}

std::wstring ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb &&
        static_cast<unsigned char>(bytes[2]) == 0xbf) {
        bytes.erase(0, 3);
    }
    if (bytes.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                     static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                        static_cast<int>(bytes.size()), result.data(), length);
    return result;
}

bool WriteTextFileAtomic(const std::filesystem::path& path, std::wstring_view text) {
    std::filesystem::create_directories(path.parent_path());
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (length < 0 || length > 65536) return false;
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        utf8.data(), length, nullptr, nullptr);
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(utf8.data(), utf8.size());
    output.close();
    return MoveFileEx(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::wstring JsonEscape(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 8);
    for (wchar_t character : value) {
        switch (character) {
        case L'\\': result += L"\\\\"; break;
        case L'"': result += L"\\\""; break;
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (character >= 0x20) result += character;
            break;
        }
    }
    return result;
}

std::wstring CurrentUserPipeName() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return L"\\\\.\\pipe\\TaskbarWidgets.RenderHost." + std::to_wstring(session);
}

std::wstring ShellHtml() {
    return LR"HTML(<!doctype html>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; frame-src https://*.taskbarwidgets.local; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:">
<style>
html,body{margin:0;width:100%;height:100%;overflow:hidden;background:transparent}
.surface{position:fixed;overflow:visible;pointer-events:auto;contain:layout style}
iframe{width:100%;height:100%;border:0;background:transparent;display:block}
</style>
<div id="root"></div>
<script>
(() => {
  'use strict';
  const surfaces = new Map();
  const safe = value => String(value || '').replace(/[^a-zA-Z0-9._-]/g, '');
  function send(frame, event, payload) {
    frame.contentWindow?.postMessage({source:'taskbar-widgets-host',event,payload}, '*');
  }
  function applyGeometry(item, animate) {
    const m=item.model, r=item.expanded?m.expandedRect:m;
    const key=[r.x,r.y,r.width,r.height].join(':');
    if (item.geometryKey===key) return;
    // Commit the WebView size once. Animating width/height makes Chromium
    // re-layout and raster the entire iframe on every transition frame.
    item.node.getAnimations().forEach(animation=>animation.cancel());
    item.node.style.transition='none';
    item.node.style.left=r.x+'px'; item.node.style.top=r.y+'px';
    item.node.style.width=r.width+'px'; item.node.style.height=r.height+'px';
    item.geometryKey=key;
    if (animate && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
      item.node.animate(
        [{opacity:.72},{opacity:1}],
        {duration:110,easing:'cubic-bezier(.2,.8,.2,1)'}
      );
    }
  }
  function setExpanded(item, expanded) {
    clearTimeout(item.timer);
    if (item.expanded===expanded) return;
    item.expanded=expanded;
    applyGeometry(item,true);
    const m=item.model;
    chrome.webview.postMessage({command:'surfaceState',instanceId:m.instanceId,expanded});
    send(item.frame,'lifecycle',{state:expanded?'expanded':'collapsed',visible:true,webglAllowed:m.webglAllowed===true,continuousAnimationAllowed:m.continuousAnimationAllowed===true});
  }
  window.__twHost = {
    upsert(m) {
      if (!m || !safe(m.instanceId) || !/^https:\/\/[a-z0-9.-]+\.taskbarwidgets\.local\//.test(m.url)) return;
      let item=surfaces.get(m.instanceId);
      if (!item) {
        const node=document.createElement('div');
        node.className='surface'; node.dataset.instance=m.instanceId;
        const frame=document.createElement('iframe');
        frame.sandbox='allow-scripts';
        frame.allow='none';
        frame.referrerPolicy='no-referrer';
        node.append(frame); document.getElementById('root').append(node);
        item={node,frame,model:m,timer:0,expanded:false,geometryKey:''};
        surfaces.set(m.instanceId,item);
        node.addEventListener('mouseenter',()=> {
          if (item.model.activation!=='hover') return;
          clearTimeout(item.timer);
          item.timer=setTimeout(()=>setExpanded(item,true),item.model.hoverDelayMs||0);
        });
        node.addEventListener('mouseleave',()=> {
          if (item.model.activation!=='hover') return;
          clearTimeout(item.timer);
          item.timer=setTimeout(()=>setExpanded(item,false),item.model.collapseDelayMs||0);
        });
        frame.src=m.url;
      }
      item.model=m; item.node.hidden=!m.visible;
      applyGeometry(item,false);
    },
    removeMissing(ids) {
      const keep=new Set(ids||[]);
      for(const [id,item] of surfaces) if(!keep.has(id)){item.node.remove();surfaces.delete(id)}
    },
    snapshot(instanceId,payload) {
      const item=surfaces.get(instanceId); if(item) send(item.frame,'snapshot',payload);
    },
    storageResult(instanceId,payload) {
      const item=surfaces.get(instanceId); if(item) send(item.frame,'storage',payload);
    },
    setting(instanceId,payload) {
      const item=surfaces.get(instanceId); if(item) send(item.frame,'settings',payload);
    },
    throttle() {
      for(const item of surfaces.values()) {
        item.node.style.transition='none';
        send(item.frame,'lifecycle',{state:item.expanded?'expanded':'collapsed',visible:!item.node.hidden,resourceThrottled:true,webglAllowed:false,continuousAnimationAllowed:false});
      }
    },
    collapseAll() {
      for(const item of surfaces.values()) setExpanded(item,false);
    }
  };
  addEventListener('message', event => {
    const message=event.data;
    if(!message || message.source!=='taskbar-widget-sdk') return;
    const item=[...surfaces.values()].find(x=>x.frame.contentWindow===event.source);
    if(!item) return;
    if(message.command==='requestSurface') setExpanded(item,message.value==='expanded');
    else if(message.command==='openSettings')
      chrome.webview.postMessage({command:'openSettings',instanceId:item.model.instanceId,widgetId:item.model.widgetId});
    else if(message.command==='storage') {
      if(!item.model.storageAllowed) {
        send(item.frame,'storage',{requestId:message.value?.requestId,ok:false,error:'Storage permission was not granted'});
      } else {
        chrome.webview.postMessage({command:'storage',instanceId:item.model.instanceId,...message.value});
      }
    }
    else if(message.command==='invoke') {
      const action=String(message.value?.action||'');
      if(/^[a-zA-Z0-9._:-]{1,120}$/.test(action)) {
        chrome.webview.postMessage({
          command:'communityInvoke',
          instanceId:item.model.instanceId,
          widgetId:item.model.widgetId,
          action,
          value:message.value?.arguments ?? {}
        });
      }
    }
    else if(message.command==='dragStart')
      chrome.webview.postMessage({command:'dragStart',instanceId:item.model.instanceId});
    else if(message.command==='ready') {
      send(item.frame,'lifecycle',{state:item.expanded?'expanded':'collapsed',visible:!item.node.hidden,webglAllowed:item.model.webglAllowed===true,continuousAnimationAllowed:item.model.continuousAnimationAllowed===true});
      chrome.webview.postMessage({command:'widgetReady',instanceId:item.model.instanceId});
    }
  });
})();
</script>)HTML";
}

std::wstring SdkScript() {
    return LR"JS((() => {
if (window.top === window || window.taskbarWidget) return;
const listeners={snapshot:new Set(),settings:new Set(),lifecycle:new Set()};
const storageRequests=new Map();
const nativeInterval=window.setInterval.bind(window);
const nativeRaf=window.requestAnimationFrame.bind(window);
let lastFrame=0,interactiveUntil=0;
let allowWebgl=false,allowContinuousAnimation=false;
window.setInterval=(callback,delay,...args)=>nativeInterval(callback,allowContinuousAnimation?(Number(delay)||0):Math.max(1000,Number(delay)||0),...args);
const nativeGetContext=HTMLCanvasElement.prototype.getContext;
HTMLCanvasElement.prototype.getContext=function(type,...args){
 if(!allowWebgl && /^(webgl2?|experimental-webgl)$/i.test(String(type)))return null;
 return nativeGetContext.call(this,type,...args);
};
window.requestAnimationFrame=callback=>nativeRaf(now=>{
 const minimum=allowContinuousAnimation||now<interactiveUntil?15:33;
 if(now-lastFrame>=minimum){lastFrame=now;callback(now)}
 else nativeRaf(later=>{lastFrame=later;callback(later)})
});
const emit=(name,value)=>listeners[name]?.forEach(fn=>{try{fn(value)}catch{}});
addEventListener('message',event=>{
 const m=event.data;
 if(m?.source==='taskbar-widgets-host' && m.event==='storage'){
   const pending=storageRequests.get(m.payload?.requestId);
   if(pending){storageRequests.delete(m.payload.requestId);m.payload.ok?pending.resolve(m.payload.value):pending.reject(new Error(m.payload.error||'Storage failed'))}
 } else if(m?.source==='taskbar-widgets-host' && listeners[m.event]){
   if(m.event==='lifecycle'){interactiveUntil=performance.now()+500;allowWebgl=m.payload?.webglAllowed===true;allowContinuousAnimation=m.payload?.continuousAnimationAllowed===true}
   emit(m.event,m.payload);
 }
});
const command=(name,value)=>parent.postMessage({source:'taskbar-widget-sdk',command:name,value},'*');
addEventListener('pointerdown',event=>{
 if(event.button===0 && event.target?.closest?.('[data-taskbar-widget-drag]')){
   event.preventDefault(); command('dragStart');
 }
},{capture:true});
let storageSequence=0;
const storage=(operation,key,value)=>new Promise((resolve,reject)=>{
 const requestId=String(++storageSequence);
 storageRequests.set(requestId,{resolve,reject});
 command('storage',{requestId,operation,key,value});
});
window.taskbarWidget=Object.freeze({
 ready(){command('ready');return Promise.resolve()},
 on(name,callback){if(!listeners[name]||typeof callback!=='function')throw new TypeError('Unsupported event');listeners[name].add(callback);return()=>listeners[name].delete(callback)},
 requestSurface(value){if(value!=='expanded'&&value!=='collapsed')return Promise.reject(new TypeError('Invalid surface'));command('requestSurface',value);return Promise.resolve()},
 openSettings(){command('openSettings');return Promise.resolve()},
 invoke(action,args={}){
   if(!/^[a-zA-Z0-9._:-]{1,120}$/.test(String(action)))return Promise.reject(new TypeError('Invalid action'));
   command('invoke',{action:String(action),arguments:args});return Promise.resolve();
 },
 storage:Object.freeze({
   async get(key){if(!/^[a-zA-Z0-9._-]{1,80}$/.test(key))throw new TypeError('Invalid key');return storage('get',key)},
   async set(key,value){if(!/^[a-zA-Z0-9._-]{1,80}$/.test(key))throw new TypeError('Invalid key');return storage('set',key,value)},
   async delete(key){if(!/^[a-zA-Z0-9._-]{1,80}$/.test(key))throw new TypeError('Invalid key');return storage('delete',key)}
 })
});
// Declarative surface controls keep working even if the widget's own script
// fails before it attaches listeners.
addEventListener('pointerdown',event=>{
 const control=event.target instanceof Element
   ? event.target.closest('[data-taskbar-widget-surface]')
   : null;
 const value=control?.getAttribute('data-taskbar-widget-surface');
 if(value==='expanded'||value==='collapsed'){
   event.preventDefault();
   command('requestSurface',value);
 }
},{capture:true});
Object.defineProperty(window,'open',{value:()=>null});
window.fetch=()=>Promise.reject(new TypeError('Direct network access is disabled'));
window.XMLHttpRequest=undefined; window.WebSocket=undefined; window.EventSource=undefined;
command('ready');
})();)JS";
}

struct Surface;
void SendSnapshotToSurface(Surface& surface, const std::wstring& instanceId);

struct Surface {
    struct WidgetHit {
        RECT rect{};
        std::wstring instanceId;
        std::wstring widgetId;
        bool expandedSurface = false;
    };
    struct WidgetModel {
        std::wstring json;
        std::wstring hostName;
        std::wstring packagePath;
    };
    HWND window = nullptr;
    RECT monitor{};
    ComPtr<IDCompositionDevice> compositionDevice;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> rootVisual;
    ComPtr<ICoreWebView2CompositionController> compositionController;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    std::vector<RECT> hitRects;
    std::vector<WidgetHit> widgetHits;
    std::unordered_set<std::wstring> expandedInstances;
    DWORD explorerProcessId = 0;
    std::chrono::steady_clock::time_point lastSeen = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastBatch = lastSeen;
    bool ready = false;
    bool suspended = false;
    bool dragging = false;
    bool dragMoved = false;
    bool foregroundOccluded = false;
    POINT dragOrigin{};
    std::wstring draggingInstance;
    std::unordered_map<std::wstring, WidgetModel> widgetModels;
    bool documentReady = false;

    void UpdateWindowRegion() {
        if (!window) return;
        RECT client{};
        if (!GetClientRect(window, &client)) return;
        HRGN combined = CreateRectRgn(0, 0, 0, 0);
        if (!combined) return;
        for (const auto& hit : widgetHits) {
            const bool active = !hit.expandedSurface ||
                expandedInstances.contains(hit.instanceId);
            if (!active) continue;
            RECT clipped{};
            if (!IntersectRect(&clipped, &hit.rect, &client)) continue;
            HRGN part = CreateRectRgn(
                clipped.left, clipped.top, clipped.right, clipped.bottom);
            if (!part) continue;
            CombineRgn(combined, combined, part, RGN_OR);
            DeleteObject(part);
        }
        // SetWindowRgn takes ownership only when it succeeds.
        if (!SetWindowRgn(window, combined, TRUE)) {
            DeleteObject(combined);
        }
    }

    void InitializeWebView() {
        if (!g_environment || !window || controller) return;
        ComPtr<ICoreWebView2Environment3> environment3;
        const HRESULT environmentResult = g_environment.As(&environment3);
        if (FAILED(environmentResult)) return;
        environment3->CreateCoreWebView2CompositionController(
            window,
            Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                [this](HRESULT error, ICoreWebView2CompositionController* value) -> HRESULT {
                    if (FAILED(error) || !value) return S_OK;
                    compositionController = value;
                    const HRESULT controllerResult = compositionController.As(&controller);
                    const HRESULT webViewResult = controller
                        ? controller->get_CoreWebView2(&webView)
                        : E_POINTER;
                    if (!controller || !webView) return S_OK;
                    RECT client{};
                    GetClientRect(window, &client);
                    const HRESULT boundsResult = controller->put_Bounds(client);
                    const HRESULT visibleResult = controller->put_IsVisible(TRUE);
                    (void)controllerResult;
                    (void)webViewResult;
                    (void)boundsResult;
                    (void)visibleResult;
                    ComPtr<ICoreWebView2Controller2> controller2;
                    if (SUCCEEDED(controller.As(&controller2))) {
                        COREWEBVIEW2_COLOR transparent{0, 0, 0, 0};
                        controller2->put_DefaultBackgroundColor(transparent);
                    }
                    CreateCompositionTree();
                    Harden();
                    webView->AddScriptToExecuteOnDocumentCreated(SdkScript().c_str(), nullptr);
                    EventRegistrationToken navigationToken{};
                    webView->add_NavigationCompleted(
                        Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [this](ICoreWebView2*,
                                   ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                documentReady = true;
                                ReplayModels();
                                return S_OK;
                            }).Get(),
                        &navigationToken);
                    webView->NavigateToString(ShellHtml().c_str());
                    ready = true;
                    return S_OK;
                }).Get());
    }

    bool CreateCompositionTree() {
        D3D_FEATURE_LEVEL level{};
        ComPtr<ID3D11Device> d3d;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                     nullptr, 0, D3D11_SDK_VERSION, &d3d, &level, nullptr))) return false;
        ComPtr<IDXGIDevice> dxgi;
        if (FAILED(d3d.As(&dxgi))) return false;
        if (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&compositionDevice)))) return false;
        if (FAILED(compositionDevice->CreateTargetForHwnd(window, TRUE, &compositionTarget))) return false;
        if (FAILED(compositionDevice->CreateVisual(&rootVisual))) return false;
        if (FAILED(compositionTarget->SetRoot(rootVisual.Get()))) return false;
        if (FAILED(compositionController->put_RootVisualTarget(rootVisual.Get()))) return false;
        return SUCCEEDED(compositionDevice->Commit());
    }

    void Harden() {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webView->get_Settings(&settings))) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_IsBuiltInErrorPageEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Settings4> settings4;
        if (SUCCEEDED(settings.As(&settings4))) {
            settings4->put_IsGeneralAutofillEnabled(FALSE);
            settings4->put_IsPasswordAutosaveEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        ComPtr<ICoreWebView2_19> webView19;
        if (SUCCEEDED(webView.As(&webView19))) {
            webView19->put_MemoryUsageTargetLevel(COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
        }
        EventRegistrationToken token{};
        webView->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    args->put_Handled(TRUE); return S_OK;
                }).Get(), &token);
        webView->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                    args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
                }).Get(), &token);
        webView->AddWebResourceRequestedFilter(
            L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webView->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    LPWSTR uriRaw = nullptr;
                    if (FAILED(args->get_Request(&request)) || !request ||
                        FAILED(request->get_Uri(&uriRaw)) || !uriRaw) return S_OK;
                    const std::wstring uri(uriRaw);
                    CoTaskMemFree(uriRaw);
                    const auto pathStart = uri.starts_with(L"https://")
                        ? uri.find(L'/', 8)
                        : std::wstring::npos;
                    const std::wstring host = pathStart == std::wstring::npos
                        ? L""
                        : uri.substr(8, pathStart - 8);
                    constexpr std::wstring_view suffix = L".taskbarwidgets.local";
                    const bool localWidget =
                        host.size() > suffix.size() && host.ends_with(suffix);
                    const bool hostDocument = uri.starts_with(L"data:") ||
                                              uri.starts_with(L"about:blank");
                    if (localWidget || hostDocument) return S_OK;
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    g_environment->CreateWebResourceResponse(
                        nullptr, 403, L"Forbidden", L"Content-Type: text/plain", &response);
                    args->put_Response(response.Get());
                    return S_OK;
                }).Get(), &token);
        webView->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR raw = nullptr;
                    if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
                    std::wstring message(raw);
                    CoTaskMemFree(raw);
                    HandleWebMessage(message);
                    return S_OK;
                }).Get(), &token);
    }

    void HandleWebMessage(const std::wstring& message) {
        namespace json = winrt::Windows::Data::Json;
        try {
            auto object = json::JsonObject::Parse(message);
            const std::wstring command = object.GetNamedString(L"command", L"").c_str();
            const std::wstring instance = object.GetNamedString(L"instanceId", L"").c_str();
            if (instance.empty() || instance.size() > 160 ||
                instance.find_first_not_of(
                    L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                    std::wstring::npos) return;
            if (command == L"openSettings") {
                const std::wstring widget = object.GetNamedString(L"widgetId", L"").c_str();
                WriteHostCommand(L"openSettings", widget);
                return;
            }
            if (command == L"surfaceState") {
                if (object.GetNamedBoolean(L"expanded", false)) {
                    expandedInstances.insert(instance);
                } else {
                    expandedInstances.erase(instance);
                }
                UpdateWindowRegion();
                return;
            }
            if (command == L"widgetReady") {
                SendSnapshotToSurface(*this, instance);
                return;
            }
            if (command == L"dragStart") {
                dragging = true;
                dragMoved = false;
                draggingInstance = instance;
                GetCursorPos(&dragOrigin);
                SetCapture(window);
                return;
            }
            if (command == L"communityInvoke") {
                const std::wstring widget =
                    object.GetNamedString(L"widgetId", L"").c_str();
                const std::wstring action =
                    object.GetNamedString(L"action", L"").c_str();
                if (widget.empty() || widget.size() > 160 ||
                    action.empty() || action.size() > 120 ||
                    action.find_first_not_of(
                        L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:-") !=
                        std::wstring::npos) {
                    return;
                }
                json::JsonObject arguments;
                arguments.Insert(L"instanceId", json::JsonValue::CreateStringValue(instance));
                arguments.Insert(L"action", json::JsonValue::CreateStringValue(action));
                if (object.HasKey(L"value")) {
                    arguments.Insert(L"value", object.GetNamedValue(L"value"));
                } else {
                    arguments.Insert(L"value", json::JsonObject());
                }
                WriteHostCommand(
                    L"communityInvoke",
                    widget,
                    std::nullopt,
                    std::nullopt,
                    std::wstring(arguments.Stringify().c_str()));
                return;
            }
            if (command != L"storage") return;
            const std::wstring requestId = object.GetNamedString(L"requestId", L"").c_str();
            const std::wstring operation = object.GetNamedString(L"operation", L"").c_str();
            const std::wstring key = object.GetNamedString(L"key", L"").c_str();
            if (requestId.empty() || requestId.size() > 40 || key.empty() || key.size() > 80 ||
                key.find_first_not_of(
                    L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                    std::wstring::npos) {
                SendStorageResult(instance, requestId, false, L"null", L"Invalid storage request");
                return;
            }
            const auto path = std::filesystem::path(g_storageDirectory) / (instance + L".json");
            json::JsonObject storage;
            const std::wstring existing = ReadTextFile(path);
            if (!existing.empty()) storage = json::JsonObject::Parse(existing);
            if (operation == L"get") {
                const std::wstring value = storage.HasKey(key)
                    ? std::wstring(storage.GetNamedValue(key).Stringify().c_str())
                    : L"null";
                SendStorageResult(instance, requestId, true, value, L"");
            } else if (operation == L"set" && object.HasKey(L"value")) {
                storage.SetNamedValue(key, object.GetNamedValue(L"value"));
                const std::wstring serialized = storage.Stringify().c_str();
                if (serialized.size() > 65536 || !WriteTextFileAtomic(path, serialized)) {
                    SendStorageResult(instance, requestId, false, L"null",
                                      L"Widget storage quota exceeded");
                } else {
                    SendStorageResult(instance, requestId, true, L"null", L"");
                }
            } else if (operation == L"delete") {
                storage.Remove(key);
                if (!WriteTextFileAtomic(path, storage.Stringify().c_str())) {
                    SendStorageResult(instance, requestId, false, L"null", L"Storage write failed");
                } else {
                    SendStorageResult(instance, requestId, true, L"null", L"");
                }
            }
        } catch (...) {
        }
    }

    void SendStorageResult(const std::wstring& instance,
                           const std::wstring& requestId,
                           bool ok,
                           const std::wstring& value,
                           const std::wstring& error) {
        Execute(L"window.__twHost&&window.__twHost.storageResult(\"" +
                JsonEscape(instance) + L"\",{requestId:\"" + JsonEscape(requestId) +
                L"\",ok:" + (ok ? L"true" : L"false") + L",value:" + value +
                L",error:\"" + JsonEscape(error) + L"\"});");
    }

    void WriteHostCommand(const std::wstring& action,
                          const std::wstring& widgetId,
                          std::optional<int> positionPercent = std::nullopt,
                          std::optional<int> offsetPixels = std::nullopt,
                          std::optional<std::wstring> argumentsJson = std::nullopt) {
        const auto directory = std::filesystem::path(g_dataDirectory) / L"Commands";
        std::filesystem::create_directories(directory);
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        const auto name = std::to_wstring(now) + L"_" +
                          std::to_wstring(GetCurrentProcessId()) + L"_" +
                          std::to_wstring(GetTickCount64()) + L".json";
        std::wstring json =
            L"{\"schemaVersion\":1,\"commandId\":\"web-" +
            std::to_wstring(now) + L"-" + std::to_wstring(GetTickCount64()) +
            L"\",\"action\":\"" + JsonEscape(action) +
            L"\",\"widgetId\":\"" + JsonEscape(widgetId) +
            L"\",\"createdAtUnix\":" + std::to_wstring(now);
        if (positionPercent) {
            json += L",\"positionPct\":" + std::to_wstring(*positionPercent);
        }
        if (offsetPixels) {
            json += L",\"offsetPx\":" + std::to_wstring(*offsetPixels);
        }
        if (argumentsJson) {
            json += L",\"arguments\":" + *argumentsJson;
        }
        json += L"}";
        WriteTextFileAtomic(directory / name, json);
    }

    void Execute(const std::wstring& script) {
        if (!documentReady || !webView) return;
        webView->ExecuteScript(script.c_str(), nullptr);
    }

    void ApplyModel(const WidgetModel& model) {
        if (!webView) return;
        if (!model.hostName.empty() && !model.packagePath.empty()) {
            ComPtr<ICoreWebView2_3> webView3;
            if (SUCCEEDED(webView.As(&webView3))) {
                webView3->SetVirtualHostNameToFolderMapping(
                    model.hostName.c_str(),
                    model.packagePath.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
            }
        }
        Execute(L"window.__twHost&&window.__twHost.upsert(" + model.json + L");");
    }

    void ReplayModels() {
        for (const auto& [_, model] : widgetModels) {
            ApplyModel(model);
        }
    }

    void UpsertModel(const std::wstring& instanceId,
                     const std::wstring& json,
                     const std::wstring& hostName,
                     const std::wstring& packagePath) {
        auto& model = widgetModels[instanceId];
        model.json = json;
        model.hostName = hostName;
        model.packagePath = packagePath;
        ApplyModel(model);
    }

    void SuspendIfHidden(bool hidden) {
        if (!controller || !webView) return;
        if (hidden == suspended) return;
        controller->put_IsVisible(hidden ? FALSE : TRUE);
        if (hidden) {
            ComPtr<ICoreWebView2_3> webView3;
            if (SUCCEEDED(webView.As(&webView3))) webView3->TrySuspend(nullptr);
        } else {
            ComPtr<ICoreWebView2_3> webView3;
            if (SUCCEEDED(webView.As(&webView3))) webView3->Resume();
        }
        suspended = hidden;
    }
};

std::map<std::wstring, std::unique_ptr<Surface>> g_surfaces;
void WriteHealth(std::wstring_view status, std::wstring_view error);
std::chrono::steady_clock::time_point g_lastResourceCheck{};
std::chrono::steady_clock::time_point g_budgetExceededSince{};
unsigned g_budgetBreaches = 0;
bool g_quarantined = false;
unsigned long long g_previousCpuTicks = 0;
std::chrono::steady_clock::time_point g_previousCpuAt{};

bool IsProcessAlive(DWORD processId) {
    if (!processId) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

bool IsForegroundCoveringMonitor(const RECT& monitor, DWORD explorerProcessId) {
    HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground)) {
        return false;
    }
    foreground = GetAncestor(foreground, GA_ROOT);
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0 || processId == explorerProcessId ||
        processId == GetCurrentProcessId()) {
        return false;
    }
    RECT bounds{};
    if (!GetWindowRect(foreground, &bounds)) return false;
    constexpr LONG tolerance = 2;
    return bounds.left <= monitor.left + tolerance &&
           bounds.top <= monitor.top + tolerance &&
           bounds.right >= monitor.right - tolerance &&
           bounds.bottom >= monitor.bottom - tolerance;
}

bool IsSafeInstanceId(std::wstring_view value) {
    return !value.empty() && value.size() <= 160 &&
        value.find_first_not_of(
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
            std::wstring_view::npos;
}

void SendSnapshotToSurface(Surface& surface, const std::wstring& instanceId) {
    if (!surface.webView || !IsSafeInstanceId(instanceId)) return;
    const auto snapshotPath = std::filesystem::path(g_dataDirectory) /
        L"State" / (instanceId + L".json");
    const std::wstring snapshot = ReadTextFile(snapshotPath);
    if (snapshot.empty() || snapshot.size() > 65536) return;
    surface.Execute(
        L"window.__twHost&&window.__twHost.snapshot(\"" +
        JsonEscape(instanceId) + L"\"," + snapshot + L");");
}

unsigned long long FileTimeTicks(const FILETIME& value) {
    ULARGE_INTEGER number{};
    number.LowPart = value.dwLowDateTime;
    number.HighPart = value.dwHighDateTime;
    return number.QuadPart;
}

void EvaluateResourceBudget() {
    const auto now = std::chrono::steady_clock::now();
    if (g_lastResourceCheck.time_since_epoch().count() != 0 &&
        now - g_lastResourceCheck < std::chrono::seconds(5)) return;
    g_lastResourceCheck = now;
    if (!g_environment || g_surfaces.empty()) return;
    UINT32 browserPid = 0;
    auto firstWebView = g_surfaces.begin()->second->webView;
    if (!firstWebView ||
        FAILED(firstWebView->get_BrowserProcessId(&browserPid)) ||
        browserPid == 0) return;

    std::unordered_set<DWORD> processIds{browserPid};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        bool changed = true;
        while (changed) {
            changed = false;
            PROCESSENTRY32 entry{sizeof(entry)};
            if (Process32First(snapshot, &entry)) {
                do {
                    if (!processIds.contains(entry.th32ProcessID) &&
                        processIds.contains(entry.th32ParentProcessID)) {
                        processIds.insert(entry.th32ProcessID);
                        changed = true;
                    }
                } while (Process32Next(snapshot, &entry));
            }
        }
        CloseHandle(snapshot);
    }

    SIZE_T workingSet = 0;
    unsigned long long cpuTicks = 0;
    for (DWORD pid : processIds) {
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!process) continue;
        PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)};
        if (GetProcessMemoryInfo(process,
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                 sizeof(memory))) {
            workingSet += memory.WorkingSetSize;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            cpuTicks += FileTimeTicks(kernel) + FileTimeTicks(user);
        }
        CloseHandle(process);
    }

    double cpuPercent = 0;
    if (g_previousCpuAt.time_since_epoch().count() != 0 &&
        cpuTicks >= g_previousCpuTicks) {
        const double elapsed100ns =
            std::chrono::duration<double>(now - g_previousCpuAt).count() * 10'000'000.0;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        cpuPercent = 100.0 * (cpuTicks - g_previousCpuTicks) /
            std::max(1.0, elapsed100ns * std::max<DWORD>(1, info.dwNumberOfProcessors));
    }
    g_previousCpuTicks = cpuTicks;
    g_previousCpuAt = now;

    std::unordered_set<std::wstring> instances;
    for (const auto& [_, surface] : g_surfaces) {
        for (const auto& hit : surface->widgetHits) {
            instances.insert(hit.instanceId);
        }
    }
    const SIZE_T memoryBudget =
        g_surfaces.size() > 1
            ? 340ULL * 1024 * 1024
            : instances.size() > 1
                ? 260ULL * 1024 * 1024
                : 220ULL * 1024 * 1024;
    if (workingSet > memoryBudget && cpuPercent < 1.0) {
        // Chromium keeps reclaimable caches resident after first navigation.
        // Ask Windows to trim those idle working sets before treating the
        // widget as a sustained resource-budget violation.
        for (DWORD pid : processIds) {
            HANDLE process = OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA,
                FALSE, pid);
            if (process) {
                EmptyWorkingSet(process);
                CloseHandle(process);
            }
        }
        return;
    }
    const bool exceeded = workingSet > memoryBudget || cpuPercent > 5.0;
    if (!exceeded) {
        g_budgetExceededSince = {};
        return;
    }
    if (g_budgetExceededSince.time_since_epoch().count() == 0) {
        g_budgetExceededSince = now;
        return;
    }
    if (now - g_budgetExceededSince < std::chrono::seconds(30)) return;
    g_budgetExceededSince = now;
    ++g_budgetBreaches;
    for (auto& [_, surface] : g_surfaces) {
        surface->Execute(L"window.__twHost&&window.__twHost.throttle();");
        ComPtr<ICoreWebView2_19> webView19;
        if (surface->webView && SUCCEEDED(surface->webView.As(&webView19))) {
            webView19->put_MemoryUsageTargetLevel(
                COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
        }
    }
    WriteHealth(g_budgetBreaches >= 2 ? L"quarantined" : L"throttled",
                L"Web widget resource budget was exceeded.");
    if (g_budgetBreaches >= 2) {
        g_quarantined = true;
        for (auto& [_, surface] : g_surfaces) {
            surface->SuspendIfHidden(true);
            ShowWindow(surface->window, SW_HIDE);
        }
    }
}

void SuspendStaleSurfaces() {
    for (auto& [_, surface] : g_surfaces) {
        const bool stale = g_quarantined ||
            (surface->explorerProcessId != 0 &&
             !IsProcessAlive(surface->explorerProcessId));
        const bool hidden = stale || surface->foregroundOccluded;
        surface->SuspendIfHidden(hidden);
        if (hidden) {
            surface->hitRects.clear();
            if (stale) {
                surface->widgetHits.clear();
                surface->expandedInstances.clear();
                surface->UpdateWindowRegion();
            }
            ShowWindow(surface->window, SW_HIDE);
        } else {
            SetWindowPos(surface->window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(surface->window, SW_SHOWNOACTIVATE);
        }
    }
    EvaluateResourceBudget();
}

void UpdateForegroundOcclusion() {
    for (auto& [_, surface] : g_surfaces) {
        const bool occluded = IsForegroundCoveringMonitor(
            surface->monitor, surface->explorerProcessId);
        if (occluded == surface->foregroundOccluded) continue;
        surface->foregroundOccluded = occluded;
        const bool stale = surface->explorerProcessId != 0 &&
                           !IsProcessAlive(surface->explorerProcessId);
        if (occluded) {
            surface->expandedInstances.clear();
            surface->UpdateWindowRegion();
            surface->Execute(L"window.__twHost&&window.__twHost.collapseAll();");
            surface->SuspendIfHidden(true);
            SetWindowPos(surface->window, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(surface->window, SW_HIDE);
        } else if (!stale && !g_quarantined) {
            surface->SuspendIfHidden(false);
            SetWindowPos(surface->window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(surface->window, SW_SHOWNOACTIVATE);
        }
    }
}

void WriteHealth(std::wstring_view status, std::wstring_view error = {}) {
    std::filesystem::create_directories(std::filesystem::path(g_dataDirectory) / L"Runtime");
    auto path = std::filesystem::path(g_dataDirectory) / L"Runtime" / L"web-render-host.json";
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    std::wstring json = L"{\"schemaVersion\":1,\"status\":\"" + JsonEscape(status) +
                        L"\",\"error\":\"" + JsonEscape(error) + L"\"}";
    int length = WideCharToMultiByte(CP_UTF8, 0, json.data(), static_cast<int>(json.size()),
                                     nullptr, 0, nullptr, nullptr);
    std::string utf8(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, json.data(), static_cast<int>(json.size()),
                        utf8.data(), length, nullptr, nullptr);
    output.write(utf8.data(), utf8.size());
    output.close();
    MoveFileEx(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void PipeServer(HWND notificationWindow) {
    const std::wstring pipeName = CurrentUserPipeName();
    while (!g_stopping) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        SECURITY_ATTRIBUTES security{sizeof(security)};
        if (ConvertStringSecurityDescriptorToSecurityDescriptor(
                L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
            security.lpSecurityDescriptor = descriptor;
        }
        HANDLE pipe = CreateNamedPipe(
            pipeName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, kMaximumPipeMessage, kMaximumPipeMessage, 1000,
            descriptor ? &security : nullptr);
        if (descriptor) LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            DWORD length = 0;
            DWORD read = 0;
            if (ReadFile(pipe, &length, sizeof(length), &read, nullptr) &&
                read == sizeof(length) && length > 0 && length <= kMaximumPipeMessage) {
                std::string bytes(length, '\0');
                if (ReadFile(pipe, bytes.data(), length, &read, nullptr) && read == length) {
                    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                                    static_cast<int>(bytes.size()), nullptr, 0);
                    if (count > 0) {
                        std::wstring message(count, L'\0');
                        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                            static_cast<int>(bytes.size()), message.data(), count);
                        {
                            std::lock_guard lock(g_pipeMutex);
                            g_pipeMessages.push_back(std::move(message));
                        }
                        PostMessage(notificationWindow, kPipeMessage, 0, 0);
                    }
                }
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Surface* surface = reinterpret_cast<Surface*>(GetWindowLongPtr(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCHITTEST:
        if (surface) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            for (const auto& hit : surface->widgetHits) {
                const bool active = !hit.expandedSurface ||
                    surface->expandedInstances.contains(hit.instanceId);
                if (active && PtInRect(&hit.rect, point)) return HTCLIENT;
            }
        }
        return HTTRANSPARENT;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (surface && surface->compositionController) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (message == WM_MOUSEMOVE && surface->dragging) {
                POINT screen = point;
                ClientToScreen(window, &screen);
                const int thresholdX = std::max(4, GetSystemMetrics(SM_CXDRAG));
                const int thresholdY = std::max(4, GetSystemMetrics(SM_CYDRAG));
                surface->dragMoved =
                    std::abs(screen.x - surface->dragOrigin.x) >= thresholdX ||
                    std::abs(screen.y - surface->dragOrigin.y) >= thresholdY;
            }
            if (message == WM_LBUTTONUP && surface->dragging) {
                if (surface->dragMoved) {
                    POINT screen = point;
                    ClientToScreen(window, &screen);
                    const int monitorWidth =
                        std::max(1L, surface->monitor.right - surface->monitor.left);
                    const int percent = std::clamp(
                        static_cast<int>(std::lround(
                            100.0 * (screen.x - surface->monitor.left) / monitorWidth)),
                        0, 100);
                    surface->WriteHostCommand(
                        L"moveWidget", surface->draggingInstance, percent, 0);
                }
                const bool moved = surface->dragMoved;
                surface->dragging = false;
                surface->dragMoved = false;
                surface->draggingInstance.clear();
                ReleaseCapture();
                if (moved) return 0;
            }
            if (message == WM_RBUTTONUP) {
                auto found = std::find_if(
                    surface->widgetHits.begin(), surface->widgetHits.end(),
                    [surface, &point](const Surface::WidgetHit& hit) {
                        const bool active = !hit.expandedSurface ||
                            surface->expandedInstances.contains(hit.instanceId);
                        return active && PtInRect(&hit.rect, point) != FALSE;
                    });
                if (found != surface->widgetHits.end()) {
                    HMENU menu = CreatePopupMenu();
                    AppendMenu(menu, MF_STRING, 1, L"Widget settings");
                    AppendMenu(menu, MF_STRING, 2, L"Disable widget");
                    POINT screen = point;
                    ClientToScreen(window, &screen);
                    const UINT choice = TrackPopupMenu(
                        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                        screen.x, screen.y, 0, window, nullptr);
                    DestroyMenu(menu);
                    if (choice == 1) surface->WriteHostCommand(L"openSettings", found->widgetId);
                    if (choice == 2) surface->WriteHostCommand(L"disableWidget", found->instanceId);
                    return 0;
                }
            }
            COREWEBVIEW2_MOUSE_EVENT_KIND kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
            if (message == WM_LBUTTONDOWN) kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
            if (message == WM_LBUTTONUP) kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
            if (message == WM_RBUTTONDOWN) kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN;
            if (message == WM_RBUTTONUP) kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP;
            surface->compositionController->SendMouseInput(
                kind, COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, 0, point);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
    case WM_TIMER:
        if (wParam == kSnapshotTimer) {
            SuspendStaleSurfaces();
            return 0;
        }
        if (wParam == kVisibilityTimer) {
            UpdateForegroundOcclusion();
            return 0;
        }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(window, message, wParam, lParam);
}

HWND CreateOverlay(const RECT& bounds) {
    HWND window = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST,
        kWindowClass, L"", WS_POPUP, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    if (window) {
        HRGN emptyRegion = CreateRectRgn(0, 0, 0, 0);
        if (emptyRegion && !SetWindowRgn(window, emptyRegion, FALSE)) {
            DeleteObject(emptyRegion);
        }
        SetWindowPos(window, HWND_TOPMOST, bounds.left, bounds.top,
                     bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    return window;
}

void CreateEnvironment(HWND notificationWindow) {
    auto userData = std::filesystem::path(g_dataDirectory) / L"WebView2";
    std::filesystem::create_directories(userData);
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(
        L"--renderer-process-limit=1 "
        L"--disable-background-networking "
        L"--disable-component-update "
        L"--disable-default-apps "
        L"--disable-domain-reliability "
        L"--disable-extensions "
        L"--disable-sync "
        L"--metrics-recording-only "
        L"--no-first-run "
        L"--disable-features=OptimizationHints,MediaRouter");
    HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [notificationWindow](HRESULT error, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(error) || !environment) {
                    WriteHealth(L"runtime-required", L"Microsoft Edge WebView2 Runtime is unavailable.");
                    return S_OK;
                }
                g_environment = environment;
                WriteHealth(L"ready");
                for (auto& [_, surface] : g_surfaces) surface->InitializeWebView();
                InvalidateRect(notificationWindow, nullptr, FALSE);
                return S_OK;
            }).Get());
    if (FAILED(result)) WriteHealth(L"runtime-required", L"WebView2 environment could not start.");
}

void ProcessPipeMessages() {
    std::vector<std::wstring> messages;
    {
        std::lock_guard lock(g_pipeMutex);
        messages.swap(g_pipeMessages);
    }
    // Geometry JSON is deliberately validated in the trusted shell as well.
    // A monitor overlay is created lazily when the hook first publishes it.
    for (const auto& json : messages) {
        auto findIntAfter = [&json](std::wstring_view key, size_t from, int fallback) {
            const std::wstring marker = L"\"" + std::wstring(key) + L"\":";
            auto at = json.find(marker, from);
            if (at == std::wstring::npos) return fallback;
            wchar_t* end = nullptr;
            long value = wcstol(json.c_str() + at + marker.size(), &end, 10);
            return end == json.c_str() + at + marker.size() ? fallback : static_cast<int>(value);
        };
        auto findInt = [&findIntAfter](std::wstring_view key, int fallback) {
            return findIntAfter(key, 0, fallback);
        };
        auto findString = [&json](std::wstring_view key) {
            const std::wstring marker = L"\"" + std::wstring(key) + L"\":\"";
            auto at = json.find(marker);
            if (at == std::wstring::npos) return std::wstring{};
            at += marker.size();
            auto end = json.find(L'"', at);
            return end == std::wstring::npos ? std::wstring{} : json.substr(at, end - at);
        };
        if (findString(L"type") == L"snapshot") {
            const std::wstring instanceId = findString(L"instanceId");
            if (IsSafeInstanceId(instanceId)) {
                for (auto& [_, surface] : g_surfaces) {
                    SendSnapshotToSurface(*surface, instanceId);
                }
            }
            continue;
        }
        const std::wstring monitorId = findString(L"monitorId");
        RECT monitor{findInt(L"monitorX", 0), findInt(L"monitorY", 0),
                     findInt(L"monitorRight", 0), findInt(L"monitorBottom", 0)};
        if (monitorId.empty() || monitor.right <= monitor.left || monitor.bottom <= monitor.top) continue;
        auto& surface = g_surfaces[monitorId];
        if (!surface) {
            surface = std::make_unique<Surface>();
            surface->monitor = monitor;
            surface->window = CreateOverlay(monitor);
            SetWindowLongPtr(surface->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(surface.get()));
            surface->InitializeWebView();
        }
        surface->explorerProcessId =
            static_cast<DWORD>(std::max(0, findInt(L"explorerPid", 0)));
        const auto now = std::chrono::steady_clock::now();
        if (now - surface->lastBatch > std::chrono::milliseconds(500)) {
            surface->hitRects.clear();
            surface->widgetHits.clear();
            surface->lastBatch = now;
        }
        surface->lastSeen = now;
        const int localX = findInt(L"x", 0);
        const int localY = findInt(L"y", 0);
        const int width = findInt(L"width", 0);
        const int height = findInt(L"height", 0);
        RECT requestedHit{
            localX, localY,
            localX + std::max(0, width),
            localY + std::max(0, height)};
        RECT clientBounds{
            0, 0,
            monitor.right - monitor.left,
            monitor.bottom - monitor.top};
        RECT hit{};
        if (!IntersectRect(&hit, &requestedHit, &clientBounds)) continue;
        surface->hitRects.push_back(hit);
        surface->widgetHits.push_back(Surface::WidgetHit{
            hit, findString(L"instanceId"), findString(L"widgetId"), false});
        const auto expandedAt = json.find(L"\"expandedRect\":");
        if (expandedAt != std::wstring::npos) {
            RECT expanded{
                findIntAfter(L"x", expandedAt, hit.left),
                findIntAfter(L"y", expandedAt, hit.top),
                0,
                0
            };
            expanded.right = expanded.left +
                findIntAfter(L"width", expandedAt, hit.right - hit.left);
            expanded.bottom = expanded.top +
                findIntAfter(L"height", expandedAt, hit.bottom - hit.top);
            RECT clippedExpanded{};
            if (IntersectRect(&clippedExpanded, &expanded, &clientBounds)) {
                surface->hitRects.push_back(clippedExpanded);
                surface->widgetHits.push_back(Surface::WidgetHit{
                    clippedExpanded,
                    findString(L"instanceId"),
                    findString(L"widgetId"),
                    true});
            }
        }
        surface->UpdateWindowRegion();
        const std::wstring hostName = findString(L"hostName");
        const std::wstring packagePath = findString(L"packagePath");
        surface->UpsertModel(
            findString(L"instanceId"), json, hostName, packagePath);
        SendSnapshotToSurface(*surface, findString(L"instanceId"));
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    g_dataDirectory = ReadArgument(argc, argv, L"--data-dir");
    g_communityDirectory = ReadArgument(argc, argv, L"--community-dir");
    g_storageDirectory = ReadArgument(argc, argv, L"--storage-dir");
    LocalFree(argv);
    if (g_dataDirectory.empty() || g_communityDirectory.empty() || g_storageDirectory.empty()) return 2;

    WNDCLASSEX windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassEx(&windowClass);

    HWND notificationWindow = CreateWindowEx(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    WriteHealth(L"starting");
    CreateEnvironment(notificationWindow);
    SetTimer(notificationWindow, kSnapshotTimer, 1000, nullptr);
    SetTimer(notificationWindow, kVisibilityTimer, 150, nullptr);
    std::thread pipeThread(PipeServer, notificationWindow);

    MSG message{};
    while (GetMessage(&message, nullptr, 0, 0) > 0) {
        if (message.message == kPipeMessage) {
            ProcessPipeMessages();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    g_stopping = true;
    // Wake a blocking ConnectNamedPipe so the worker can finish.
    HANDLE wake = CreateFile(CurrentUserPipeName().c_str(), GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    pipeThread.join();
    g_surfaces.clear();
    g_environment.Reset();
    CoUninitialize();
    return 0;
}
