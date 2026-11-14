// ao4_bugcam: bug capture for the AO4 native port (launcher option, off by default).
// Captures the game window with Windows.Graphics.Capture (no hooks in the game) and keeps the
// last N seconds in memory at half resolution (duplicate frames skipped). A capture is written to
// <out>/<timestamp>/:
//  - F12 (game window in front): screenshot.png at full resolution + the frames before/after it;
//  - automatically when a frame turns abruptly green (green dominance far above the running
//    average; armed after 20 s, at most one automatic capture per 20 s, at most 10 per session);
//  - test trigger: a file <out>/trigger.txt (removed when taken).
// Usage: ao4_bugcam <pid> <expected-executable> <output-directory> [seconds=6]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {
struct Search { DWORD pid; HWND window = nullptr; uint64_t area = 0; };
BOOL CALLBACK FindGameWindow(HWND w, LPARAM data) {
  auto& s = *reinterpret_cast<Search*>(data);
  DWORD pid{};
  GetWindowThreadProcessId(w, &pid);
  RECT r{};
  if (pid == s.pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetClientRect(w, &r)) {
    const uint64_t area = uint64_t(r.right) * r.bottom;
    if (r.right >= 320 && r.bottom >= 200 && area > s.area) { s.window = w; s.area = area; }
  }
  return TRUE;
}

void WritePng(const std::filesystem::path& path, const BYTE* pixels, UINT width, UINT height, UINT pitch) {
  com_ptr<IWICImagingFactory> factory;
  check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())));
  com_ptr<IWICStream> stream;
  check_hresult(factory->CreateStream(stream.put()));
  check_hresult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
  com_ptr<IWICBitmapEncoder> encoder;
  check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
  check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
  com_ptr<IWICBitmapFrameEncode> frame;
  check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
  check_hresult(frame->Initialize(nullptr));
  check_hresult(frame->SetSize(width, height));
  auto format = GUID_WICPixelFormat32bppBGRA;
  check_hresult(frame->SetPixelFormat(&format));
  check_hresult(frame->WritePixels(height, pitch, pitch * height, const_cast<BYTE*>(pixels)));
  check_hresult(frame->Commit());
  check_hresult(encoder->Commit());
}

struct Frame {
  std::vector<BYTE> pixels;  // half resolution BGRA
  UINT width = 0, height = 0;
  ULONGLONG tick = 0;
  double green = 0, luma = 0;
};

// Fixed ring of reused frame buffers.
struct Ring {
  std::vector<Frame> slots;
  size_t head = 0, count = 0;
  Frame& Next() {
    Frame& f = slots[(head + count) % slots.size()];
    if (count < slots.size()) ++count; else head = (head + 1) % slots.size();
    return f;
  }
  const Frame& At(size_t i) const { return slots[(head + i) % slots.size()]; }
  void DropOlderThan(ULONGLONG tick) {
    while (count && At(0).tick < tick) { head = (head + 1) % slots.size(); --count; }
  }
};

std::wstring Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t s[64];
  swprintf_s(s, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  return s;
}

std::atomic<bool> g_saving{false};

// Writes one capture in a background thread (the capture loop keeps running; one save at a time).
void SaveCapture(std::filesystem::path dir, std::vector<Frame> frames, std::vector<BYTE> full, UINT full_w,
                 UINT full_h, ULONGLONG trigger_tick, std::string reason) {
  g_saving = true;
  std::thread([dir = std::move(dir), frames = std::move(frames), full = std::move(full), full_w, full_h, trigger_tick,
               reason = std::move(reason)]() {
    try {
      init_apartment(apartment_type::multi_threaded);
      std::filesystem::create_directories(dir);
      std::ofstream index(dir / L"index.txt");
      index << "reason " << reason << "\n";
      if (!full.empty()) {
        WritePng(dir / L"screenshot.png", full.data(), full_w, full_h, full_w * 4);
        index << "screenshot.png full resolution, at the trigger\n";
      }
      int n = 0;
      for (const Frame& f : frames) {
        wchar_t name[64];
        swprintf_s(name, L"frame-%04d.png", n++);
        WritePng(dir / name, f.pixels.data(), f.width, f.height, f.width * 4);
        char line[160];
        snprintf(line, sizeof(line), "%ls t=%+lldms green=%.1f luma=%.1f\n", name,
                 static_cast<long long>(f.tick) - static_cast<long long>(trigger_tick), f.green, f.luma);
        index << line;
      }
    } catch (...) {
    }
    g_saving = false;
  }).detach();
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    init_apartment(apartment_type::multi_threaded);
    if (argc < 4) return 2;
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    const std::filesystem::path expected = std::filesystem::absolute(argv[2]);
    const std::filesystem::path out = std::filesystem::absolute(argv[3]);
    const uint32_t seconds = argc > 4 ? (std::min)(20u, (std::max)(2u, uint32_t(_wtoi(argv[4])))) : 6u;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return 1;
    wchar_t executable[32768]{};
    DWORD len = 32768;
    if (!QueryFullProcessImageNameW(process, 0, executable, &len) || _wcsicmp(executable, expected.c_str()) != 0)
      return 1;
    if (!GraphicsCaptureSession::IsSupported()) return 3;
    Search target{pid};
    const ULONGLONG wait_start = GetTickCount64();
    while (!target.window && GetTickCount64() - wait_start < 120000 && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
      EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&target));
      if (!target.window) Sleep(250);
    }
    if (!target.window) return 1;

    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    check_hresult(interop->CreateForWindow(target.window, guid_of<GraphicsCaptureItem>(), put_abi(item)));
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
    auto dxgi = device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    check_hresult(::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(inspectable.as<IDirect3DDevice>(),
                                                               DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto token = pool.FrameArrived([signal](auto&&, auto&&) { SetEvent(signal); });
    auto session = pool.CreateCaptureSession(item);
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    session.StartCapture();

    Ring ring;
    ring.slots.resize(size_t(seconds) * 36);  // the game presents 30 frames/s; duplicates are skipped
    auto size = item.Size();
    com_ptr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC staging_desc{};
    uint64_t last_hash = 0;
    double green_avg = 0;
    uint32_t frames_seen = 0;
    bool f12_down = false;
    int post_frames = -1, auto_captures = 0;
    std::string pending_reason;
    std::vector<BYTE> pending_full;
    UINT pending_w = 0, pending_h = 0;
    ULONGLONG pending_tick = 0, last_auto = 0, last_trigger_check = 0;

    std::ofstream status(out / L"bugcam.log", std::ios::app);
    status << "start pid=" << pid << " window=" << target.window << "\n" << std::flush;
    uint32_t arrived = 0, kept = 0;
    ULONGLONG last_status = GetTickCount64();
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && IsWindow(target.window)) {
      const ULONGLONG loop_now = GetTickCount64();
      if (loop_now - last_status >= 10000) {
        status << "frames arrived=" << arrived << " kept=" << kept << " ring=" << ring.count << " saving=" << g_saving
               << "\n" << std::flush;
        arrived = kept = 0;
        last_status = loop_now;
      }
      if (WaitForSingleObject(signal, 500) != WAIT_OBJECT_0) {
        // No new frame (static picture or minimized window): still honour F12 and the file trigger.
        DWORD fg{};
        GetWindowThreadProcessId(GetForegroundWindow(), &fg);
        const bool key = fg == pid && (GetAsyncKeyState(VK_F12) & 0x8000);
        std::error_code ec;
        const bool trig = (key && !f12_down) || std::filesystem::remove(out / L"trigger.txt", ec);
        f12_down = key;
        if (trig && post_frames < 0 && !g_saving && ring.count) {
          std::vector<Frame> snapshot;
          for (size_t i = 0; i < ring.count; ++i) snapshot.push_back(ring.At(i));
          status << "capture without new frames, frames=" << snapshot.size() << "\n" << std::flush;
          SaveCapture(out / Stamp(), std::move(snapshot), {}, 0, 0, loop_now, "trigger (no new frames)");
        }
        continue;
      }
      auto frame = pool.TryGetNextFrame();
      if (!frame) continue;
      ++arrived;
      auto next = frame.ContentSize();
      if (next.Width != size.Width || next.Height != size.Height) {
        frame.Close();
        size = next;
        if (size.Width > 0 && size.Height > 0)
          pool.Recreate(inspectable.as<IDirect3DDevice>(), DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        staging = nullptr;
        ring.count = 0;
        continue;
      }
      auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
      com_ptr<ID3D11Texture2D> texture;
      check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
      D3D11_TEXTURE2D_DESC desc{};
      texture->GetDesc(&desc);
      if (!staging || staging_desc.Width != desc.Width || staging_desc.Height != desc.Height) {
        staging_desc = desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.MiscFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging = nullptr;
        check_hresult(device->CreateTexture2D(&staging_desc, nullptr, staging.put()));
      }
      context->CopyResource(staging.get(), texture.get());
      frame.Close();
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) continue;
      const auto* src = static_cast<const BYTE*>(mapped.pData);
      const UINT w = desc.Width, h = desc.Height;
      const ULONGLONG now = GetTickCount64();

      // Cheap sampled hash + colour statistics; identical frames (compositor repeats) are skipped.
      uint64_t hash = 1469598103934665603ull;
      double sum_g = 0, sum_rb = 0, sum_l = 0;
      uint64_t n = 0, odd = 0;
      for (UINT y = 4; y < h; y += 8) {
        const BYTE* row = src + size_t(y) * mapped.RowPitch;
        for (UINT x = 4; x < w; x += 8) {
          const BYTE* p = row + size_t(x) * 4;
          hash = (hash ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16)) * 1099511628211ull;
          sum_g += p[1];
          sum_rb += (p[0] + p[2]) * 0.5;
          sum_l += 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0];
          ++n;
          // Strongly saturated green/cyan/blue/magenta: rare in the game (fire is red-orange), typical
          // for wrongly interpreted HDR render target data (rainbow garbage).
          const BYTE mx = (std::max)(p[0], (std::max)(p[1], p[2])), mn = (std::min)(p[0], (std::min)(p[1], p[2]));
          if (mx - mn > 110 && (p[1] == mx || p[0] == mx || (p[2] > 150 && p[0] > 150 && p[1] < 90))) ++odd;
        }
      }
      const bool duplicate = hash == last_hash;
      last_hash = hash;
      const double green = n ? (sum_g - sum_rb) / n : 0, luma = n ? sum_l / n : 0;
      const double odd_pct = n ? 100.0 * odd / n : 0;

      // Triggers.
      DWORD foreground_pid{};
      GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
      const bool f12 = foreground_pid == pid && (GetAsyncKeyState(VK_F12) & 0x8000);
      std::string reason;
      if (f12 && !f12_down) reason = "F12";
      f12_down = f12;
      if (reason.empty() && now - last_trigger_check > 500) {
        last_trigger_check = now;
        std::error_code ec;
        if (std::filesystem::remove(out / L"trigger.txt", ec)) reason = "trigger file";
      }
      if (reason.empty() && !duplicate && frames_seen > 600 && auto_captures < 10 && now - last_auto > 20000 &&
          ((green - green_avg > 30.0 && luma > 15.0) || odd_pct > 4.0)) {
        char text[128];
        snprintf(text, sizeof(text), "auto: green %.1f (running average %.1f), garbage colours %.1f%%", green, green_avg,
                 odd_pct);
        reason = text;
        last_auto = now;
        ++auto_captures;
      }
      if (!reason.empty() && post_frames < 0 && !g_saving) {
        pending_reason = reason;
        pending_tick = now;
        pending_w = w;
        pending_h = h;
        pending_full.resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y)
          memcpy(pending_full.data() + size_t(y) * w * 4, src + size_t(y) * mapped.RowPitch, size_t(w) * 4);
        post_frames = 30;  // about one second after the trigger as well
      }

      if (!duplicate) {
        ++kept;
        Frame& f = ring.Next();
        f.width = w / 2;
        f.height = h / 2;
        f.tick = now;
        f.green = green;
        f.luma = luma;
        f.pixels.resize(size_t(f.width) * f.height * 4);
        for (UINT y = 0; y < f.height; ++y) {
          const uint32_t* row = reinterpret_cast<const uint32_t*>(src + size_t(y * 2) * mapped.RowPitch);
          uint32_t* dst = reinterpret_cast<uint32_t*>(f.pixels.data()) + size_t(y) * f.width;
          for (UINT x = 0; x < f.width; ++x) dst[x] = row[x * 2];
        }
        ring.DropOlderThan(now - uint64_t(seconds) * 1000);
        green_avg = frames_seen ? green_avg * 0.97 + green * 0.03 : green;
        ++frames_seen;
        if (post_frames > 0) --post_frames;
      }
      context->Unmap(staging.get(), 0);

      if (post_frames == 0) {
        std::vector<Frame> snapshot;
        snapshot.reserve(ring.count);
        for (size_t i = 0; i < ring.count; ++i) snapshot.push_back(ring.At(i));
        status << "capture " << pending_reason << " frames=" << snapshot.size() << "\n" << std::flush;
        SaveCapture(out / Stamp(), std::move(snapshot), std::move(pending_full), pending_w, pending_h, pending_tick,
                    pending_reason);
        pending_full = {};
        post_frames = -1;
      }
    }
    pool.FrameArrived(token);
    session.Close();
    pool.Close();
    CloseHandle(signal);
    CloseHandle(process);
    for (int i = 0; i < 600 && g_saving; ++i) Sleep(100);  // let a pending save finish
    return 0;
  } catch (...) {
    return 1;
  }
}
