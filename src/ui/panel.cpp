#include "ui/panel.hpp"

#include "core/util.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <dwmapi.h>
#include <dxgi1_3.h>

#include <map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace sp {
namespace {

ImVec4 hexv(uint32_t c, float a = 1.f) {
  return ImVec4(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a);
}

// Same colors the spider paints harvested text with, so the list reads at a glance.
ImVec4 kind_color(const std::string& k) {
  static const std::map<std::string, uint32_t> m = {
      {"sentence", 0xE64CF2}, {"heading", 0xE64CF2}, {"title", 0xF2836B}, {"doi", 0x93F5AE},
      {"isbn", 0x7D8BFF},     {"id", 0x4E8FF0},      {"link", 0x7D8BFF},  {"url", 0x7FD6FF},
      {"email", 0x7FD6FF},    {"date", 0xFFC46B},    {"number", 0xFF5C5C}, {"handle", 0x7FD6FF},
      {"item", 0xC8CCD8}};
  auto it = m.find(k);
  return hexv(it == m.end() ? 0xC8CCD8 : it->second);
}

void style(float k) {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0;
  s.ChildRounding = 6;
  s.FrameRounding = 5;
  s.GrabRounding = 5;
  s.PopupRounding = 6;
  s.WindowPadding = ImVec2(16, 14);
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(8, 8);
  s.ScrollbarSize = 10;
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = hexv(0x0E0F14);
  c[ImGuiCol_ChildBg] = hexv(0x15161D);
  c[ImGuiCol_PopupBg] = hexv(0x1B1C25);
  c[ImGuiCol_Text] = hexv(0xE9EBF2);
  c[ImGuiCol_TextDisabled] = hexv(0x8A8FA3);
  c[ImGuiCol_FrameBg] = hexv(0x1E2029);
  c[ImGuiCol_FrameBgHovered] = hexv(0x272A36);
  c[ImGuiCol_FrameBgActive] = hexv(0x2E3240);
  c[ImGuiCol_Button] = hexv(0x22242F);
  c[ImGuiCol_ButtonHovered] = hexv(0x2F3242);
  c[ImGuiCol_ButtonActive] = hexv(0x3A3E52);
  c[ImGuiCol_CheckMark] = hexv(0x8BF5A6);
  c[ImGuiCol_SliderGrab] = hexv(0xEE7A68);
  c[ImGuiCol_SliderGrabActive] = hexv(0xF29585);
  c[ImGuiCol_Header] = hexv(0x22242F);
  c[ImGuiCol_HeaderHovered] = hexv(0x2A2D3B);
  c[ImGuiCol_HeaderActive] = hexv(0x333748);
  c[ImGuiCol_Separator] = hexv(0xFFFFFF, 0.08f);
  c[ImGuiCol_ScrollbarBg] = hexv(0x000000, 0);
  c[ImGuiCol_ScrollbarGrab] = hexv(0x2E3140);
  c[ImGuiCol_Border] = hexv(0xFFFFFF, 0.06f);
  c[ImGuiCol_Header] = hexv(0x1A1B23);
  s.ScaleAllSizes(k);
}

void push(ImFont* f) { ImGui::PushFont(f, f ? f->LegacySize : 0.f); }

}  // namespace

bool Panel::create(HINSTANCE inst, bool quiet, const POINT* at) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &Panel::proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  wc.hbrBackground = CreateSolidBrush(RGB(0x0E, 0x0F, 0x14));
  wc.lpszClassName = L"SpiderPetPanel";
  RegisterClassExW(&wc);

  POINT origin{90, 90};
  UINT dpi = 96;
  if (HMONITOR m = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY)) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    origin = {mi.rcWork.left + 90, mi.rcWork.top + 90};
  }
  if (at) origin = *at;
  hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"SpiderPet", WS_OVERLAPPEDWINDOW, origin.x, origin.y, 440, 720,
                          nullptr, nullptr, inst, this);
  if (!hwnd_) return false;
  dpi = GetDpiForWindow(hwnd_);
  scale_ = dpi / 96.f;
  SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(440 * scale_), static_cast<int>(720 * scale_),
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
  const COLORREF caption = 0x00140F0E;
  DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof(caption));  // DWMWA_CAPTION_COLOR

  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device_,
                               nullptr, &ctx_)))
    return false;
  ComPtr<IDXGIDevice> dxgi;
  device_.As(&dxgi);
  ComPtr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  adapter->GetParent(IID_PPV_ARGS(&factory));
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swap_))) return false;
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  make_target();

  IMGUI_CHECKVERSION();
  imgui_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(imgui_);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  body_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 16.f * scale_, &cfg);
  title_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 23.f * scale_, &cfg);
  small_ = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 13.5f * scale_, &cfg);
  if (!body_) body_ = io.Fonts->AddFontDefault();
  if (!title_) title_ = body_;
  if (!small_) small_ = body_;
  style(scale_);
  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX11_Init(device_.Get(), ctx_.Get());

  // Quiet (tests): never take focus; minimized unless a position was given.
  ShowWindow(hwnd_, quiet ? (at ? SW_SHOWNOACTIVATE : SW_SHOWMINNOACTIVE) : SW_SHOW);
  if (!quiet) UpdateWindow(hwnd_);
  return true;
}

void Panel::make_target() {
  rtv_.Reset();
  ComPtr<ID3D11Texture2D> back;
  if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
}

void Panel::destroy() {
  if (imgui_) {
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(imgui_);
    imgui_ = nullptr;
  }
  rtv_.Reset();
  swap_.Reset();
  ctx_.Reset();
  device_.Reset();
  if (hwnd_) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
}

LRESULT CALLBACK Panel::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
  }
  auto* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->hwnd_ == hwnd) return self->handle(msg, wp, lp);
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Panel::handle(UINT msg, WPARAM wp, LPARAM lp) {
  if (imgui_) {
    ImGui::SetCurrentContext(imgui_);
    if (ImGui_ImplWin32_WndProcHandler(hwnd_, msg, wp, lp)) return TRUE;
  }
  switch (msg) {
    case WM_SIZE:
      minimized_ = wp == SIZE_MINIMIZED;
      if (swap_ && !minimized_) {
        rtv_.Reset();
        swap_->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
        make_target();
      }
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = {static_cast<LONG>(340 * scale_), static_cast<LONG>(420 * scale_)};
      return 0;
    }
    case WM_CLOSE:
      closed_ = true;
      return 0;
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Panel::notice(const std::string& text) {
  notice_ = text;
  notice_at_ = now_seconds();
}

PanelActions Panel::draw(const PanelModel& m, Settings& s) {
  PanelActions a;
  if (!hwnd_ || minimized_ || IsIconic(hwnd_) || !rtv_) return a;
  ImGui::SetCurrentContext(imgui_);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

  // Name and what it is doing.
  push(title_);
  ImGui::TextUnformatted(utf8(m.name).c_str());
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4 * scale_);
  if (ImGui::SmallButton("new name")) a.reroll = true;
  push(small_);
  ImGui::TextColored(hexv(0x8BF5A6), "%s", m.state.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("on %s", m.where.empty() ? "the desktop" : utf8(m.where).c_str());
  ImGui::PopFont();

  if (!m.gist.empty()) {
    ImGui::Spacing();
    ImGui::TextColored(hexv(0x8FD0FF), "\"%s\"", m.gist.c_str());
  }
  if (!m.mind.empty()) {
    // What it has been thinking, newest first.
    push(small_);
    for (size_t i = 0; i < m.mind.size() && i < 5; ++i) {
      const auto& [at, text] = m.mind[i];
      const int ago = static_cast<int>(m.now - at);
      ImGui::TextColored(i == 0 ? hexv(0xE9EBF2) : hexv(0x8A8FA3), "%s", utf8(text).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled(ago < 60 ? "%ds" : "%dm", ago < 60 ? ago : ago / 60);
    }
    ImGui::PopFont();
  }
  if (!m.reader_ready) ImGui::TextColored(hexv(0xFF5C5C), "Windows text reading (OCR) is not available.");
  if (!m.attached) {
    push(small_);
    ImGui::TextColored(hexv(0xFFC46B), "Drag the spider onto any window. It reads what is there.");
    ImGui::PopFont();
  } else {
    push(small_);
    ImGui::TextDisabled("%d things found here, %d harvested", m.found, m.harvested);
    ImGui::PopFont();
  }

  ImGui::Spacing();
  if (m.attached && ImGui::Button("Call it back")) a.recall = true;
  if (m.attached) ImGui::SameLine();
  if (ImGui::Button("Clear marks")) a.clear_marks = true;

  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Settings")) {
    a.settings_changed |= ImGui::Checkbox("Scroll the page by itself", &s.crawl);
    a.settings_changed |= ImGui::Checkbox("Read key sentences", &s.read);
    a.settings_changed |= ImGui::Checkbox("Spin silk between finds", &s.web);
    a.settings_changed |= ImGui::Checkbox("Short summaries from local AI", &s.brain);
    if (s.brain) {
      ImGui::SameLine();
      push(small_);
      ImGui::TextDisabled("(%s)", m.brain.c_str());
      ImGui::PopFont();
    }
    a.settings_changed |= ImGui::Checkbox("Show the spider in screen shares", &s.share);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    a.settings_changed |= ImGui::SliderFloat("Size", &s.size, 0.5f, 2.f, "%.2fx");
  }

  // Harvest.
  const auto& recs = m.store->all();
  ImGui::SeparatorText("Harvest");
  std::map<std::string, int> counts;
  for (const Record& r : recs) counts[r.kind]++;
  push(small_);
  if (ImGui::SmallButton(("all " + std::to_string(recs.size())).c_str())) filter_ = -1;
  int i = 0;
  std::string pick;
  for (auto& [k, n] : counts) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < 70 * scale_) ImGui::NewLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kind_color(k));
    if (ImGui::SmallButton((k + " " + std::to_string(n)).c_str())) filter_ = i;
    ImGui::PopStyleColor();
    if (filter_ == i) pick = k;
    ++i;
  }
  ImGui::PopFont();

  const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
  ImGui::BeginChild("list", ImVec2(0, -footer), ImGuiChildFlags_None);
  if (recs.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("  Nothing harvested yet.");
  }
  const float tag_w = 80 * scale_;
  // Each row: click to be taken to it on the page, click a link to open it,
  // right-click for copy and open.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (int idx = static_cast<int>(recs.size()) - 1; idx >= 0; --idx) {
    const Record& r = recs[idx];
    if (!pick.empty() && r.kind != pick) continue;
    ImGui::PushID(idx);
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::BeginGroup();
    push(small_);
    ImGui::TextColored(kind_color(r.kind), "%s", (r.label.empty() ? r.kind : r.label).c_str());
    ImGui::PopFont();
    ImGui::SameLine(tag_w);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(r.text.c_str());
    bool link_clicked = false;
    if (!r.url.empty()) {
      ImGui::SetCursorPosX(tag_w);
      push(small_);
      ImGui::TextColored(hexv(0x7D8BFF), "%s", r.url.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        dl->AddLine({lo.x, hi.y}, hi, ImGui::GetColorU32(hexv(0x7D8BFF)));
        if (ImGui::IsMouseClicked(0)) link_clicked = true;
      }
      ImGui::PopFont();
    }
    if (!r.summary.empty()) {
      ImGui::SetCursorPosX(tag_w);
      push(small_);
      ImGui::TextColored(hexv(0x8FD0FF), "%s", r.summary.c_str());
      ImGui::PopFont();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    dl->ChannelsSetCurrent(0);
    if (hovered) {
      dl->AddRectFilled({lo.x - 4, lo.y - 2}, {hi.x + 4, hi.y + 2}, ImGui::GetColorU32(hexv(0x2A2D3B)), 4.f);
      ImGui::SetTooltip("click: show me where it is\nright-click: copy or open");
    }
    dl->ChannelsMerge();
    if (link_clicked) a.open_url = r.url;
    else if (hovered && ImGui::IsMouseClicked(0)) a.show = idx;
    if (ImGui::BeginPopupContextItem("row")) {
      if (ImGui::MenuItem("Show on the page")) a.show = idx;
      if (ImGui::MenuItem("Copy text")) a.clip = r.text;
      if (!r.url.empty() && ImGui::MenuItem("Open link")) a.open_url = r.url;
      if (!r.url.empty() && ImGui::MenuItem("Copy link")) a.clip = r.url;
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();

  if (ImGui::Button("Save JSON")) a.save_json = true;
  ImGui::SameLine();
  if (ImGui::Button("Save CSV")) a.save_csv = true;
  ImGui::SameLine();
  if (ImGui::Button("Copy")) a.copy = true;
  ImGui::SameLine();
  if (ImGui::Button("Folder")) a.open_folder = true;
  ImGui::SameLine();
  if (ImGui::Button("Clear")) a.clear_list = true;
  push(small_);
  if (!notice_.empty() && now_seconds() - notice_at_ < 6) ImGui::TextColored(hexv(0x8BF5A6), "%s", notice_.c_str());
  else ImGui::TextDisabled("Alt+Shift+S: drop it under the mouse. Drag its bubble to pin it; double-click to free it.");
  ImGui::PopFont();

  ImGui::End();
  ImGui::Render();
  const float clear[4] = {0x0E / 255.f, 0x0F / 255.f, 0x14 / 255.f, 1.f};
  ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
  ctx_->ClearRenderTargetView(rtv_.Get(), clear);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  swap_->Present(0, 0);
  return a;
}

}  // namespace sp
