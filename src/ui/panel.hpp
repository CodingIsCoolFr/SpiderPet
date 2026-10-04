#pragma once

#include "app/store.hpp"
#include "pet/pet.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <utility>
#include <vector>

struct ImGuiContext;
struct ImFont;

namespace sp {

struct PanelModel {
  std::wstring name;
  std::string state;
  std::wstring where;
  std::wstring thought;
  std::vector<std::pair<double, std::wstring>> mind;  // recent thoughts, newest first
  double now = 0;
  std::string gist;
  std::string brain;
  bool reader_ready = true;
  bool attached = false;
  int found = 0;
  int harvested = 0;
  const Store* store = nullptr;
};

struct PanelActions {
  bool reroll = false;
  bool recall = false;
  bool clear_marks = false;
  bool save_json = false;
  bool save_csv = false;
  bool copy = false;
  bool open_folder = false;
  bool clear_list = false;
  bool settings_changed = false;
  int show = -1;          // record index: take me to it on the page
  std::string open_url;   // open in the browser
  std::string clip;       // put on the clipboard
};

// The control window: a plain Win32 window drawn with Dear ImGui on D3D11.
class Panel {
 public:
  bool create(HINSTANCE inst, bool quiet, const POINT* at = nullptr);
  void destroy();
  HWND hwnd() const { return hwnd_; }
  bool closed() const { return closed_; }
  PanelActions draw(const PanelModel& m, Settings& s);
  void notice(const std::string& text);

 private:
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT, WPARAM, LPARAM);
  void make_target();

  HWND hwnd_ = nullptr;
  bool closed_ = false;
  bool minimized_ = false;
  float scale_ = 1;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  ImGuiContext* imgui_ = nullptr;
  ImFont* body_ = nullptr;
  ImFont* title_ = nullptr;
  ImFont* small_ = nullptr;
  int filter_ = -1;
  std::string notice_;
  double notice_at_ = 0;
};

}  // namespace sp
