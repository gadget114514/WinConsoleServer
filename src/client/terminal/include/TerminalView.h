#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "TerminalBuffer.h"
#include "TerminalEmulator.h"
#include "ImageManager.h"
#include "../../iterminal_io.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

// Posted by the I/O backend when output is queued for the UI thread.
#define WM_TERMINAL_OUTPUT      (WM_USER + 200)
#define WM_TERMINAL_OUTPUT_WAKE (WM_USER + 201)
// Posted by the WCSP receiver thread when the server reports an applied size.
// lParam is a heap-allocated wcs::ResizedInfo owned by the window procedure.
#define WM_TERMINAL_RESIZED     (WM_USER + 202)

// Sent to parent window to report taskbar progress (lParam = 0–10000, or -1 to clear).
#define WM_TERMINAL_PROGRESS (WM_USER + 299)

// ---------------------------------------------------------------------------
// TerminalView
//
// A self-contained Win32 child window that embeds a full terminal emulator.
// Rendering uses Direct2D / DirectWrite (matching Ecode's existing pipeline).
//
// Usage:
//   TerminalView* tv = new TerminalView();
//   HWND hwnd = tv->Create(parentHwnd);
//   tv->StartSession(L"powershell.exe", {});
// ---------------------------------------------------------------------------
class TerminalView {
public:
    TerminalView();
    ~TerminalView();

    // Create the child HWND (hidden by default).  Returns nullptr on failure.
    HWND Create(HWND parent);

    // Install the I/O backend.  Must be called before StartSession().
    // If not called, a LocalSession (ConPTY) is created automatically.
    void SetIo(std::unique_ptr<ITerminalIo> io);

    // Lazy-start the session.  Safe to call multiple times.
    bool StartSession(const TerminalStartOptions& opt, std::wstring* error = nullptr);
    bool StartSession(
        const std::wstring& shell = L"powershell.exe",
        UINT codePage = CP_UTF8,
        const std::vector<std::pair<std::wstring,std::wstring>>& extraEnv = {});

    bool IsStarted() const { return io_ && io_->IsRunning(); }
    HWND Hwnd()      const { return hwnd_; }
    int  Columns()   const { return buffer_.columns(); }
    int  Rows()      const { return buffer_.rows(); }
    HANDLE GetProcessHandle() const { return nullptr; }
    std::wstring LastError()  const { return io_ ? io_->LastError() : std::wstring(); }

    // Feed raw PTY output.  Must be called on the UI thread.
    void FeedOutput(const char* data, size_t len);

    // Taskbar progress via ITaskbarList3 (requires CoInitializeEx).
    void setProgressCallback(std::function<void(float)> cb) {
        onProgress_ = std::move(cb);
        emulator_.setProgressCallback(onProgress_);
    }

    // Called from parent's WM_SIZE handler.
    void MoveAndResize(int x, int y, int w, int h);

    // Send raw text/bytes to the PTY.
    void SendInput(const std::string& utf8);
    void SendInput(const std::wstring& text);

    // Window class name for RegisterClassEx
    static constexpr const wchar_t* kClassName = L"EcodeTerminalView";
    static bool RegisterWindowClass(HINSTANCE hInst);

    // Clipboard (also invoked by the host menu via WM_COPY/WM_CUT/WM_PASTE)
    void PasteFromClipboard();
    void CopySelectionToClipboard() const;
    void CutSelectionToClipboard();

private:
    // Win32 window procedure (dispatches to instance method)
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    // Message handlers
    LRESULT OnCreate(HWND hwnd);
    void    OnDestroy();
    void    OnSize(int w, int h);
    void    OnPaint();
    void    OnKeyDown(WPARAM vk, LPARAM lParam);
    void    OnChar(wchar_t ch);
    void    OnMouseWheel(int delta);
    void    OnTerminalOutput(const char* data, size_t len);
    void    EnqueueOutput(const char* data, size_t len);
    void    DrainOutputQueue();
    void    OnRemoteResized(const wcs::ResizedInfo& info);
    void    ReassertSize();
    bool    GridSizeFor(int w, int h, int& cols, int& rows) const;

    // Rendering
    bool EnsureRenderTarget();
    void ReleaseRenderTarget();
    void ReleaseBrushes();
    void InitTextFormats();
    void UpdateMetrics();
    void UpdateDpiScale(UINT dpi = 0);
    float ScalePx(float value) const { return value * dpiScale_; }
    float ScrollBarWidth() const { return kScrollBarWidth * dpiScale_; }
    void DrawCell(ID2D1RenderTarget* rt, int row, int col,
                  const TerminalCell& cell, bool isCursor, bool cursorVisible,
                  bool isSelected = false);
    ID2D1SolidColorBrush* GetBrush(const TermColor& c, float alpha = 1.0f);
    D2D1_COLOR_F ToD2DColor(const TermColor& c, float alpha = 1.0f);

    // VT key encoding
    std::string EncodeKey(WPARAM vk, bool ctrl, bool shift, bool alt);

    // Clipboard
    void OnContextMenu(int screenX, int screenY);

    // Mouse selection
    void OnLButtonDown(int px, int py);
    void OnMouseMove(int px, int py);
    void OnLButtonUp();
    void BufferCoordFromPoint(int px, int py, int& outRow, int& outCol) const;
    bool IsSelected(int logRow, int col) const;

    // Mouse
    void SendMouseWheelToPty(int direction, int px, int py); // direction: +1=up -1=down
    void SendMouseClickToPty(int button, int px, int py, bool release);

    // Scrollback
    static constexpr int kScrollBarWidth = 8;
    int  scrollOffset_ = 0;  // lines scrolled back (0 = bottom)
    int  maxScrollOffset_ = 0; // cached historyLineCount at last paint
    bool scrolledBack_ = false; // true when scrollOffset_ > 0
    // Set when another client resized the shared PTY; this window reasserts
    // its own size on the next focus or key input.
    bool sizeMismatch_ = false;
    void ScrollBy(int lines);
    void ScrollToBottom();
    int  VisibleRows() const;
    void DrawScrollBar(ID2D1RenderTarget* rt);
    void DrawScrollIndicator(ID2D1RenderTarget* rt, const RECT& clientRc);

    // Cursor blink
    void StartCursorTimer();
    void StopCursorTimer();

    // --- OSC 1337 image handlers ---
    void OnImageData(const std::vector<uint8_t>& data,
                     int widthPx, int heightPx,
                     bool preserveAspectRatio,
                     const std::wstring& name);
    void OnFileDownload(const std::vector<uint8_t>& data,
                        const std::wstring& name);

    // --- Sixel handler ---
    void OnSixelData(const std::vector<uint8_t>& data);

    // --- state ---
    HWND hwnd_ = nullptr;

    TerminalBuffer  buffer_;
    TerminalEmulator emulator_;
    std::unique_ptr<ITerminalIo> io_;

    // D2D / DWrite
    ID2D1Factory*          d2dFactory_  = nullptr;
    ID2D1HwndRenderTarget* renderTarget_= nullptr;
    IDWriteFactory*        dwFactory_   = nullptr;
    IDWriteTextFormat*     textFormat_  = nullptr;  // regular
    IDWriteTextFormat*     textFormatBold_       = nullptr;
    IDWriteTextFormat*     textFormatItalic_     = nullptr;
    IDWriteTextFormat*     textFormatBoldItalic_ = nullptr;

    // colour + alpha → brush cache (brushes live until the render target is recreated)
    std::unordered_map<uint32_t, ID2D1SolidColorBrush*> brushCache_;

    // Window DPI scale (physical pixels per DIP). The render target is created
    // at 96 DPI so Direct2D units are physical pixels; text formats and small
    // drawing offsets are scaled by this factor instead.
    float dpiScale_ = 1.0f;

    // Cell metrics (physical pixels)
    float cellWidth_  = 8.0f;
    float cellHeight_ = 16.0f;
    float baseline_   = 12.0f;

    // Cursor blink
    bool   cursorBlink_   = true;   // current visible state (true = visible; starts visible so cursor shows at startup)
    UINT_PTR cursorTimer_ = 0;

    // SGR 5 text blink
    bool   textBlinkOn_   = true;   // current visible phase
    bool   hasBlinkCells_ = false;  // set during paint; controls full-window invalidation

    // Output queue: I/O thread pushes, UI thread drains (WM_TERMINAL_OUTPUT_WAKE).
    std::mutex outputMutex_;
    std::deque<std::vector<uint8_t>> outputQueue_;
    size_t outputQueueBytes_ = 0;
    std::atomic<bool> outputWakePosted_{false};
    static constexpr size_t kOutputQueueHighWater = 4u * 1024u * 1024u;

    // Code page for MultiByteToWideChar/WideCharToMultiByte (from settings.ini / --codepage)
    UINT codePage_ = CP_UTF8;

    // Hyperlink config (from settings.ini [Terminal] section)
    bool clickToOpenHyperlink_ = true;
    int  hyperlinkModifier_    = 3;     // 0=Ctrl, 1=Alt, 2=Shift, 3=no modifier
    std::wstring hyperlinkOpenCommand_; // empty = ShellExecuteW, else {url} template

    // Tooltip for hyperlinks
    HWND tooltip_ = nullptr;
    bool tooltipActive_ = false;
    std::wstring tooltipText_;

    // Is there a hyperlink at the given client-area point?
    bool IsHyperlinkAt(int px, int py) const;

    // Selection state
    bool selecting_    = false;
    int  selAnchorRow_ = -1;
    int  selAnchorCol_ = -1;
    int  selEndRow_    = -1;
    int  selEndCol_    = -1;

    // IME インライン入力
    std::wstring     imeComposition_;   // 変換中の文字列（空 = 非アクティブ）
    std::vector<BYTE> imeCompAttr_;     // 各文字の属性（ATTR_INPUT/ATTR_CONVERTED）
    bool             imeActive_ = false;
    int              imeShift_ = 0;     // 編集中の右シフト量（セル単位）

    // OSC 1337 image manager
    ImageManager* imageManager_ = nullptr;

    // Progress callback (OSC 9;4)
    std::function<void(float)> onProgress_;

    // Partial multi-byte sequence buffer (跨ぐチャンク間の不完全なマルチバイトシーケンス)
    std::string pendingPartial_;

};
