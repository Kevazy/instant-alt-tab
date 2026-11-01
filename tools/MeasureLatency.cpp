// Measures how long the Alt+Tab menu takes to appear, the way a person sees it:
// presses Alt+Tab (Alt held, as in normal use), captures the screen with
// Desktop Duplication, closes the menu with Esc, and repeats.
//
//   first    keypress -> the first presented frame in which the middle of the
//            screen changed (the menu appeared)
//   settled  keypress -> the last frame, within the hold time, in which the menu
//            area still changed (thumbnails and icons drawn)
//
// usage: measure-latency [openings=20] [holdMs=700] [gapMs=1500]
// Run it with a still window in the foreground: anything animating in the
// middle of the screen (a spinner, a video) is counted as a change.

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kBoxWidth = 320, kBoxHeight = 200;  // the middle of the screen, for "first"
constexpr int kGridStep = 4;                      // sampling grid for the whole screen, for "settled"
constexpr double kBoxChange = 12.0;               // mean luminance change that counts as "the menu appeared"
constexpr int kPointChange = 24;                  // luminance change of one grid point
constexpr size_t kPointsForChange = 12;           // changed points inside the menu that count as a change

double g_msPerTick;

double Ms(LONGLONG ticks) { return ticks * g_msPerTick; }

LONGLONG Now() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void Key(WORD key, bool up) {
    INPUT input{.type = INPUT_KEYBOARD, .ki = {.wVk = key, .dwFlags = up ? KEYEVENTF_KEYUP : 0u}};
    SendInput(1, &input, sizeof(input));
}

struct Frame {
    LONGLONG presented = 0;
    std::vector<BYTE> box, grid;  // luminance samples
};

class Screen {
public:
    bool Open() {
        HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
            ComPtr<IDXGIOutput> candidate;
            for (UINT o = 0; adapter->EnumOutputs(o, &candidate) == S_OK; ++o) {
                DXGI_OUTPUT_DESC description;
                candidate->GetDesc(&description);
                if (description.Monitor == monitor) {
                    output = candidate;
                    m_width = description.DesktopCoordinates.right - description.DesktopCoordinates.left;
                    m_height = description.DesktopCoordinates.bottom - description.DesktopCoordinates.top;
                    break;
                }
            }
            if (!output) adapter.Reset();
        }
        ComPtr<IDXGIOutput1> output1;
        if (!output || FAILED(output.As(&output1)) ||
            FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &m_device, nullptr, &m_context)) ||
            FAILED(output1->DuplicateOutput(m_device.Get(), &m_duplication))) {
            return false;
        }
        D3D11_TEXTURE2D_DESC staging{.Width = static_cast<UINT>(m_width),
                                     .Height = static_cast<UINT>(m_height),
                                     .MipLevels = 1,
                                     .ArraySize = 1,
                                     .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                                     .SampleDesc = {1, 0},
                                     .Usage = D3D11_USAGE_STAGING,
                                     .CPUAccessFlags = D3D11_CPU_ACCESS_READ};
        return SUCCEEDED(m_device->CreateTexture2D(&staging, nullptr, &m_staging));
    }

    // The next presented frame; `wholeScreen` also samples the grid.
    bool Grab(UINT timeoutMs, Frame& frame, bool wholeScreen) {
        DXGI_OUTDUPL_FRAME_INFO info;
        ComPtr<IDXGIResource> resource;
        if (FAILED(m_duplication->AcquireNextFrame(timeoutMs, &info, &resource))) return false;
        frame.presented = info.LastPresentTime.QuadPart;
        ComPtr<ID3D11Texture2D> texture;
        resource.As(&texture);
        const int boxX = m_width / 2 - kBoxWidth / 2, boxY = m_height / 2 - kBoxHeight / 2;
        if (wholeScreen) {
            m_context->CopyResource(m_staging.Get(), texture.Get());
        } else {
            D3D11_BOX box{static_cast<UINT>(boxX),
                          static_cast<UINT>(boxY),
                          0,
                          static_cast<UINT>(boxX + kBoxWidth),
                          static_cast<UINT>(boxY + kBoxHeight),
                          1};
            m_context->CopySubresourceRegion(m_staging.Get(), 0, boxX, boxY, 0, texture.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            auto luminance = [&](int x, int y) {
                auto pixel = static_cast<const BYTE*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                return static_cast<BYTE>((pixel[0] + 2 * pixel[1] + pixel[2]) / 4);
            };
            frame.box.resize(kBoxWidth * kBoxHeight);
            for (int y = 0; y < kBoxHeight; ++y)
                for (int x = 0; x < kBoxWidth; ++x) frame.box[y * kBoxWidth + x] = luminance(boxX + x, boxY + y);
            if (wholeScreen) {
                frame.grid.resize(GridWidth() * GridHeight());
                for (int y = 0; y < GridHeight(); ++y)
                    for (int x = 0; x < GridWidth(); ++x)
                        frame.grid[y * GridWidth() + x] = luminance(x * kGridStep, y * kGridStep);
            }
            m_context->Unmap(m_staging.Get(), 0);
        }
        m_duplication->ReleaseFrame();
        return true;
    }

    int GridWidth() const { return m_width / kGridStep; }
    int GridHeight() const { return m_height / kGridStep; }

private:
    int m_width = 0, m_height = 0;
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIOutputDuplication> m_duplication;
    ComPtr<ID3D11Texture2D> m_staging;
};

double MeanDifference(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    if (a.empty() || a.size() != b.size()) return 0;
    long long sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
    return static_cast<double>(sum) / a.size();
}

void PrintSummary(const char* name, std::vector<double> values) {
    if (values.empty()) return;
    std::sort(values.begin(), values.end());
    printf("%s: median %.1f ms, min %.1f, max %.1f over %zu\n", name, values[values.size() / 2], values.front(),
           values.back(), values.size());
}

}  // namespace

int main(int argc, char** argv) {
    const int openings = argc > 1 ? atoi(argv[1]) : 20;
    const int holdMs = argc > 2 ? atoi(argv[2]) : 700;
    const int gapMs = argc > 3 ? atoi(argv[3]) : 1500;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_msPerTick = 1000.0 / frequency.QuadPart;

    Screen screen;
    if (!screen.Open()) {
        printf("Desktop Duplication isn't available for this monitor.\n");
        return 1;
    }
    const int gridWidth = screen.GridWidth(), gridHeight = screen.GridHeight();
    std::vector<double> firsts, settles;
    Frame before, frame;
    for (int n = 0; n < openings; ++n) {
        Sleep(gapMs);
        while (screen.Grab(0, frame, true)) before = frame;  // the screen just before the press

        Key(VK_MENU, false);
        Sleep(40);
        const LONGLONG pressed = Now();
        Key(VK_TAB, false);
        Key(VK_TAB, true);

        double first = -1, settled = -1;
        while (Ms(Now() - pressed) < 3000) {
            if (screen.Grab(100, frame, false) && frame.presented &&
                MeanDifference(frame.box, before.box) > kBoxChange) {
                first = Ms(frame.presented - pressed);
                break;
            }
        }
        if (first >= 0) {
            std::vector<Frame> frames;
            while (Ms(Now() - pressed) < holdMs) {
                if (screen.Grab(50, frame, true) && frame.presented) frames.push_back(frame);
            }
            if (!frames.empty()) {
                // The menu's area: every grid point that differs from the screen before the press.
                const auto& last = frames.back().grid;
                int x0 = gridWidth, y0 = gridHeight, x1 = -1, y1 = -1;
                for (int y = 0; y < gridHeight; ++y) {
                    for (int x = 0; x < gridWidth; ++x) {
                        if (std::abs(last[y * gridWidth + x] - before.grid[y * gridWidth + x]) > kPointChange) {
                            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                        }
                    }
                }
                for (size_t i = 0; i < frames.size() && x1 >= 0; ++i) {
                    const auto& previous = i ? frames[i - 1].grid : before.grid;
                    size_t changed = 0;
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                            changed += std::abs(frames[i].grid[y * gridWidth + x] - previous[y * gridWidth + x]) >
                                       kPointChange;
                    if (changed >= kPointsForChange) settled = Ms(frames[i].presented - pressed);
                }
            }
        }
        while (Ms(Now() - pressed) < holdMs) Sleep(10);
        Key(VK_ESCAPE, false);
        Key(VK_ESCAPE, true);
        Sleep(60);
        Key(VK_MENU, true);

        if (first < 0) {
            printf("opening %d: no menu detected\n", n + 1);
            continue;
        }
        firsts.push_back(first);
        if (settled >= 0) settles.push_back(settled);
        printf("opening %d: first %.1f ms, settled %.1f ms\n", n + 1, first, settled);
    }
    PrintSummary("first", firsts);
    PrintSummary("settled", settles);
    return 0;
}
