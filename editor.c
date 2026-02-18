// Windows-native tiny GUI text editor
// Build (MinGW): windres resource.rc -O coff -o resource.o && gcc -O2 -Wall -Wextra -std=c11 -mwindows editor.c resource.o -o editor.exe -lcomdlg32 -ld2d1
// Build (MSVC): rc resource.rc && cl /O2 editor.c resource.res user32.lib gdi32.lib comdlg32.lib d2d1.lib

#include <windows.h>
#include <commdlg.h>
#include <d2d1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>

#define ID_EDIT      100
#define ID_FILE_NEW  101
#define ID_FILE_OPEN 102
#define ID_FILE_SAVE 103
#define ID_FILE_INFO 104
#define ID_FILE_EXIT 105
#define ID_EDIT_UNDO 201
#define ID_EDIT_CUT  202
#define ID_EDIT_COPY 203
#define ID_EDIT_PASTE 204
#define ID_EDIT_DELETE 205
#define ID_EDIT_SELECT_ALL 206
#define ID_VIEW_READ_ONLY 301
#define ID_VIEW_ALWAYS_ON_TOP 302
#define ID_VIEW_WORD_WRAP 303
#define ID_FORMAT_FONT 351
#define ID_HELP_ABOUT 401
#define WM_APP_RENDER_READY (WM_APP + 1)

#define MAX_MENU_TEXTS 128

static HWND g_edit = NULL;
static HBRUSH g_bg_brush = NULL;
static HBRUSH g_editor_brush = NULL;
static HBRUSH g_header_brush = NULL;
static HBRUSH g_panel_brush = NULL;
static HPEN g_frame_pen = NULL;
static HBRUSH g_menu_bg_brush = NULL;
static HBRUSH g_menu_hot_brush = NULL;
static HFONT g_font = NULL;
static HFONT g_menu_font = NULL;
static HFONT g_header_font = NULL;
static ID2D1Factory *g_d2d_factory = NULL;
static ID2D1HwndRenderTarget *g_d2d_rt = NULL;
static ID2D1SolidColorBrush *g_d2d_bg_brush = NULL;
static ID2D1SolidColorBrush *g_d2d_header_brush = NULL;
static ID2D1SolidColorBrush *g_d2d_panel_brush = NULL;
static ID2D1SolidColorBrush *g_d2d_accent_brush = NULL;
static ID2D1SolidColorBrush *g_d2d_frame_brush = NULL;
static HANDLE g_render_thread = NULL;
static HANDLE g_render_request_event = NULL;
static HANDLE g_render_stop_event = NULL;
static CRITICAL_SECTION g_render_lock;
static BOOL g_render_lock_ready = FALSE;
static HBITMAP g_render_frame = NULL;
static int g_render_frame_w = 0;
static int g_render_frame_h = 0;
static FILE *g_log_file = NULL;
static WNDPROC g_edit_proc = NULL;
static LOGFONTA g_logfont = {0};
static char g_current_file[MAX_PATH] = "";
static char g_launch_file[MAX_PATH] = "";
static char *g_menu_texts[MAX_MENU_TEXTS] = {0};
static int g_menu_text_count = 0;
static BOOL g_read_only = FALSE;
static BOOL g_always_on_top = FALSE;
static BOOL g_word_wrap = FALSE;

static const COLORREF COLOR_BG = RGB(30, 34, 42);
static const COLORREF COLOR_HEADER_BG = RGB(20, 23, 30);
static const COLORREF COLOR_PANEL_BG = RGB(36, 40, 50);
static const COLORREF COLOR_EDITOR_BG = RGB(24, 27, 33);
static const COLORREF COLOR_TEXT = RGB(230, 233, 239);
static const COLORREF COLOR_SUBTEXT = RGB(152, 160, 176);
static const COLORREF COLOR_ACCENT = RGB(93, 145, 255);
static const COLORREF COLOR_MENU_BG = RGB(34, 38, 46);
static const COLORREF COLOR_MENU_HOT = RGB(58, 66, 84);
static const COLORREF COLOR_MENU_TEXT = RGB(230, 233, 239);
static const COLORREF COLOR_MENU_TEXT_DISABLED = RGB(140, 145, 156);
static const COLORREF COLOR_INFO_BG = RGB(24, 28, 36);
static const COLORREF COLOR_INFO_PANEL = RGB(40, 46, 58);

static void request_render(void);
static int get_skin_header_h(HWND hwnd);
static void get_editor_rect(HWND hwnd, RECT *rc);

static D2D1_COLOR_F d2d_color(COLORREF c) {
    D2D1_COLOR_F out;
    out.r = (FLOAT)GetRValue(c) / 255.0f;
    out.g = (FLOAT)GetGValue(c) / 255.0f;
    out.b = (FLOAT)GetBValue(c) / 255.0f;
    out.a = 1.0f;
    return out;
}

static void d2d_release_target(void) {
    if (g_d2d_frame_brush) { ID2D1SolidColorBrush_Release(g_d2d_frame_brush); g_d2d_frame_brush = NULL; }
    if (g_d2d_accent_brush) { ID2D1SolidColorBrush_Release(g_d2d_accent_brush); g_d2d_accent_brush = NULL; }
    if (g_d2d_panel_brush) { ID2D1SolidColorBrush_Release(g_d2d_panel_brush); g_d2d_panel_brush = NULL; }
    if (g_d2d_header_brush) { ID2D1SolidColorBrush_Release(g_d2d_header_brush); g_d2d_header_brush = NULL; }
    if (g_d2d_bg_brush) { ID2D1SolidColorBrush_Release(g_d2d_bg_brush); g_d2d_bg_brush = NULL; }
    if (g_d2d_rt) { ID2D1HwndRenderTarget_Release(g_d2d_rt); g_d2d_rt = NULL; }
}

static BOOL d2d_ensure_factory(void) {
    HRESULT hr;
    if (g_d2d_factory) return TRUE;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&g_d2d_factory);
    return SUCCEEDED(hr);
}

static BOOL d2d_ensure_target(HWND hwnd) {
    RECT rc;
    D2D1_RENDER_TARGET_PROPERTIES rt_props;
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props;
    D2D1_COLOR_F color;
    HRESULT hr;

    if (g_d2d_rt) return TRUE;
    if (!d2d_ensure_factory()) return FALSE;

    GetClientRect(hwnd, &rc);
    ZeroMemory(&rt_props, sizeof(rt_props));
    rt_props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rt_props.pixelFormat.format = DXGI_FORMAT_UNKNOWN;
    rt_props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    rt_props.dpiX = 0.0f;
    rt_props.dpiY = 0.0f;
    rt_props.usage = D2D1_RENDER_TARGET_USAGE_NONE;
    rt_props.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    ZeroMemory(&hwnd_props, sizeof(hwnd_props));
    hwnd_props.hwnd = hwnd;
    hwnd_props.pixelSize.width = (UINT32)(rc.right - rc.left);
    hwnd_props.pixelSize.height = (UINT32)(rc.bottom - rc.top);
    hwnd_props.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    hr = ID2D1Factory_CreateHwndRenderTarget(g_d2d_factory, &rt_props, &hwnd_props, &g_d2d_rt);
    if (FAILED(hr)) return FALSE;

    color = d2d_color(COLOR_BG);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(g_d2d_rt, &color, NULL, &g_d2d_bg_brush);
    if (FAILED(hr)) { d2d_release_target(); return FALSE; }
    color = d2d_color(COLOR_HEADER_BG);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(g_d2d_rt, &color, NULL, &g_d2d_header_brush);
    if (FAILED(hr)) { d2d_release_target(); return FALSE; }
    color = d2d_color(COLOR_PANEL_BG);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(g_d2d_rt, &color, NULL, &g_d2d_panel_brush);
    if (FAILED(hr)) { d2d_release_target(); return FALSE; }
    color = d2d_color(COLOR_MENU_HOT);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(g_d2d_rt, &color, NULL, &g_d2d_accent_brush);
    if (FAILED(hr)) { d2d_release_target(); return FALSE; }
    color = d2d_color(COLOR_ACCENT);
    hr = ID2D1HwndRenderTarget_CreateSolidColorBrush(g_d2d_rt, &color, NULL, &g_d2d_frame_brush);
    if (FAILED(hr)) { d2d_release_target(); return FALSE; }

    return TRUE;
}

static void d2d_resize(HWND hwnd) {
    RECT rc;
    D2D1_SIZE_U size;
    if (!g_d2d_rt) return;
    GetClientRect(hwnd, &rc);
    size.width = (UINT32)(rc.right - rc.left);
    size.height = (UINT32)(rc.bottom - rc.top);
    ID2D1HwndRenderTarget_Resize(g_d2d_rt, &size);
}

static BOOL d2d_draw_chrome(HWND hwnd) {
    RECT rc;
    int header_h;
    D2D1_RECT_F r;
    HRESULT hr;

    if (!d2d_ensure_target(hwnd)) return FALSE;
    GetClientRect(hwnd, &rc);
    header_h = get_skin_header_h(hwnd);

    ID2D1RenderTarget_BeginDraw((ID2D1RenderTarget *)g_d2d_rt);

    r.left = 0.0f; r.top = 0.0f; r.right = (FLOAT)(rc.right - rc.left); r.bottom = (FLOAT)(rc.bottom - rc.top);
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)g_d2d_rt, &r, (ID2D1Brush *)g_d2d_bg_brush);

    r.left = 0.0f; r.top = 0.0f; r.right = (FLOAT)(rc.right - rc.left); r.bottom = (FLOAT)header_h;
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)g_d2d_rt, &r, (ID2D1Brush *)g_d2d_header_brush);

    r.left = 0.0f; r.top = (FLOAT)header_h; r.right = (FLOAT)(rc.right - rc.left); r.bottom = (FLOAT)(rc.bottom - rc.top);
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)g_d2d_rt, &r, (ID2D1Brush *)g_d2d_panel_brush);

    r.left = 0.0f; r.top = (FLOAT)(header_h - 2); r.right = (FLOAT)(rc.right - rc.left); r.bottom = (FLOAT)header_h;
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget *)g_d2d_rt, &r, (ID2D1Brush *)g_d2d_accent_brush);

    {
        RECT editor_rc;
        D2D1_RECT_F frame;
        get_editor_rect(hwnd, &editor_rc);
        frame.left = (FLOAT)(editor_rc.left - 1);
        frame.top = (FLOAT)(editor_rc.top - 1);
        frame.right = (FLOAT)(editor_rc.right + 1);
        frame.bottom = (FLOAT)(editor_rc.bottom + 1);
        ID2D1RenderTarget_DrawRectangle((ID2D1RenderTarget *)g_d2d_rt, &frame, (ID2D1Brush *)g_d2d_frame_brush, 1.0f, NULL);
    }

    hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget *)g_d2d_rt, NULL, NULL);
    if (hr == D2DERR_RECREATE_TARGET) {
        d2d_release_target();
        return FALSE;
    }
    return SUCCEEDED(hr);
}

static void init_logging(void) {
    if (g_log_file) return;
    g_log_file = fopen("editor.log", "ab");
}

static void close_logging(void) {
    if (!g_log_file) return;
    fclose(g_log_file);
    g_log_file = NULL;
}

static void log_message(const char *fmt, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");

    if (g_log_file) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(
            g_log_file,
            "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond, (unsigned)st.wMilliseconds,
            buffer
        );
        fflush(g_log_file);
    }
}

typedef struct {
    char *title;
    char *message;
    HFONT title_font;
    HFONT body_font;
    HBRUSH bg_brush;
    HBRUSH panel_brush;
    HWND ok_btn;
} InfoBoxState;

enum {
    SKIN_HEADER_H = 42,
    SKIN_GAP = 12,
    WINDOW_MIN_W = 560,
    WINDOW_MIN_H = 380
};

static int get_skin_header_h(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int h = (rc.bottom - rc.top) / 9;
    if (h < SKIN_HEADER_H) h = SKIN_HEADER_H;
    if (h > 56) h = 56;
    return h;
}

static void get_editor_rect(HWND hwnd, RECT *rc) {
    int header_h = get_skin_header_h(hwnd);
    GetClientRect(hwnd, rc);
    rc->left += SKIN_GAP;
    rc->right -= SKIN_GAP;
    rc->top += header_h + SKIN_GAP;
    rc->bottom -= SKIN_GAP;
    if (rc->right < rc->left) rc->right = rc->left;
    if (rc->bottom < rc->top) rc->bottom = rc->top;
}

static void apply_skin_layout(HWND hwnd) {
    if (!g_edit) return;
    RECT rc;
    get_editor_rect(hwnd, &rc);
    MoveWindow(g_edit, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    request_render();
    InvalidateRect(hwnd, NULL, TRUE);
}

static void render_chrome(HDC hdc, int width, int height, const char *path_text) {
    RECT rect = {0, 0, width, height};
    int header_h = height / 9;
    if (header_h < SKIN_HEADER_H) header_h = SKIN_HEADER_H;
    if (header_h > 56) header_h = 56;

    FillRect(hdc, &rect, g_bg_brush);

    RECT header = rect;
    header.bottom = header_h;
    FillRect(hdc, &header, g_header_brush);

    RECT panel = rect;
    panel.top = header_h;
    FillRect(hdc, &panel, g_panel_brush);

    RECT accent = header;
    accent.top = header.bottom - 2;
    FillRect(hdc, &accent, g_menu_hot_brush);

    HFONT old_font = (HFONT)SelectObject(hdc, g_header_font ? g_header_font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COLOR_TEXT);
    int text_y = (header_h - 16) / 2;
    if (text_y < 6) text_y = 6;
    TextOutA(hdc, SKIN_GAP, text_y, "Editor", 6);

    SetTextColor(hdc, COLOR_SUBTEXT);
    TextOutA(hdc, SKIN_GAP + 72, text_y, path_text, lstrlenA(path_text));

    {
        const char *hint = "Ctrl+O Open   Ctrl+S Save   Ctrl+Shift+F Font";
        SIZE hint_sz = {0};
        GetTextExtentPoint32A(hdc, hint, lstrlenA(hint), &hint_sz);
        SetTextColor(hdc, COLOR_SUBTEXT);
        TextOutA(hdc, width - SKIN_GAP - hint_sz.cx, text_y, hint, lstrlenA(hint));
    }

    if (g_frame_pen) {
        RECT editor_rc = rect;
        editor_rc.left += SKIN_GAP;
        editor_rc.right -= SKIN_GAP;
        editor_rc.top += header_h + SKIN_GAP;
        editor_rc.bottom -= SKIN_GAP;
        if (editor_rc.right < editor_rc.left) editor_rc.right = editor_rc.left;
        if (editor_rc.bottom < editor_rc.top) editor_rc.bottom = editor_rc.top;

        HPEN old_pen = (HPEN)SelectObject(hdc, g_frame_pen);
        HBRUSH old_brush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, editor_rc.left - 1, editor_rc.top - 1, editor_rc.right + 1, editor_rc.bottom + 1);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
    }

    SelectObject(hdc, old_font);
}

static void draw_header_text(HDC hdc, int width, int header_h, const char *path_text) {
    HFONT old_font = (HFONT)SelectObject(hdc, g_header_font ? g_header_font : GetStockObject(DEFAULT_GUI_FONT));
    const char *hint = "Ctrl+O Open   Ctrl+S Save   Ctrl+Shift+F Font";
    SIZE hint_sz = {0};
    int text_y = (header_h - 16) / 2;
    if (text_y < 6) text_y = 6;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, COLOR_TEXT);
    TextOutA(hdc, SKIN_GAP, text_y, "Editor", 6);

    SetTextColor(hdc, COLOR_SUBTEXT);
    TextOutA(hdc, SKIN_GAP + 72, text_y, path_text, lstrlenA(path_text));

    GetTextExtentPoint32A(hdc, hint, lstrlenA(hint), &hint_sz);
    SetTextColor(hdc, COLOR_SUBTEXT);
    TextOutA(hdc, width - SKIN_GAP - hint_sz.cx, text_y, hint, lstrlenA(hint));

    SelectObject(hdc, old_font);
}

static HBITMAP build_render_frame(HWND hwnd, int *out_w, int *out_h) {
    RECT rc;
    HDC wnd_dc = NULL;
    HDC mem_dc = NULL;
    HBITMAP bmp = NULL;
    HBITMAP old = NULL;
    char path[MAX_PATH];

    if (!out_w || !out_h) return NULL;
    GetClientRect(hwnd, &rc);
    *out_w = rc.right - rc.left;
    *out_h = rc.bottom - rc.top;
    if (*out_w <= 0 || *out_h <= 0) {
        return NULL;
    }

    wnd_dc = GetDC(hwnd);
    if (!wnd_dc) return NULL;
    mem_dc = CreateCompatibleDC(wnd_dc);
    if (!mem_dc) {
        ReleaseDC(hwnd, wnd_dc);
        return NULL;
    }

    bmp = CreateCompatibleBitmap(wnd_dc, *out_w, *out_h);
    if (!bmp) {
        DeleteDC(mem_dc);
        ReleaseDC(hwnd, wnd_dc);
        return NULL;
    }
    old = (HBITMAP)SelectObject(mem_dc, bmp);

    lstrcpynA(path, g_current_file[0] ? g_current_file : "Untitled", MAX_PATH);
    render_chrome(mem_dc, *out_w, *out_h, path);

    SelectObject(mem_dc, old);
    DeleteDC(mem_dc);
    ReleaseDC(hwnd, wnd_dc);
    return bmp;
}

static DWORD WINAPI render_thread_proc(LPVOID param) {
    HWND hwnd = (HWND)param;
    HANDLE waits[2];
    waits[0] = g_render_stop_event;
    waits[1] = g_render_request_event;

    for (;;) {
        DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0) {
            break;
        }
        if (wr == WAIT_OBJECT_0 + 1) {
            int w = 0;
            int h = 0;
            HBITMAP frame = build_render_frame(hwnd, &w, &h);
            HBITMAP old_frame = NULL;

            if (g_render_lock_ready) {
                EnterCriticalSection(&g_render_lock);
                old_frame = g_render_frame;
                g_render_frame = frame;
                g_render_frame_w = w;
                g_render_frame_h = h;
                LeaveCriticalSection(&g_render_lock);
            } else {
                old_frame = frame;
            }
            if (old_frame) {
                DeleteObject(old_frame);
            }
            PostMessageA(hwnd, WM_APP_RENDER_READY, 0, 0);
        }
    }
    return 0;
}

static void request_render(void) {
    if (g_render_request_event) {
        SetEvent(g_render_request_event);
    }
}

static void start_render_thread(HWND hwnd) {
    if (g_render_thread) return;

    InitializeCriticalSection(&g_render_lock);
    g_render_lock_ready = TRUE;

    g_render_request_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_render_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_render_request_event || !g_render_stop_event) {
        if (g_render_request_event) CloseHandle(g_render_request_event);
        if (g_render_stop_event) CloseHandle(g_render_stop_event);
        g_render_request_event = NULL;
        g_render_stop_event = NULL;
        DeleteCriticalSection(&g_render_lock);
        g_render_lock_ready = FALSE;
        return;
    }

    g_render_thread = CreateThread(NULL, 0, render_thread_proc, hwnd, 0, NULL);
    if (!g_render_thread) {
        CloseHandle(g_render_request_event);
        CloseHandle(g_render_stop_event);
        g_render_request_event = NULL;
        g_render_stop_event = NULL;
        DeleteCriticalSection(&g_render_lock);
        g_render_lock_ready = FALSE;
        return;
    }

    request_render();
}

static void stop_render_thread(void) {
    HBITMAP frame = NULL;

    if (g_render_stop_event) {
        SetEvent(g_render_stop_event);
    }
    if (g_render_request_event) {
        SetEvent(g_render_request_event);
    }
    if (g_render_thread) {
        WaitForSingleObject(g_render_thread, 2000);
        CloseHandle(g_render_thread);
        g_render_thread = NULL;
    }
    if (g_render_request_event) {
        CloseHandle(g_render_request_event);
        g_render_request_event = NULL;
    }
    if (g_render_stop_event) {
        CloseHandle(g_render_stop_event);
        g_render_stop_event = NULL;
    }

    if (g_render_lock_ready) {
        EnterCriticalSection(&g_render_lock);
        frame = g_render_frame;
        g_render_frame = NULL;
        g_render_frame_w = 0;
        g_render_frame_h = 0;
        LeaveCriticalSection(&g_render_lock);
        if (frame) {
            DeleteObject(frame);
        }
        DeleteCriticalSection(&g_render_lock);
        g_render_lock_ready = FALSE;
    }
}

static char *dup_text(const char *text) {
    size_t n = strlen(text) + 1u;
    char *copy = (char *)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    return copy;
}

static LRESULT CALLBACK info_box_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    InfoBoxState *state = (InfoBoxState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_NCCREATE: {
            CREATESTRUCTA *cs = (CREATESTRUCTA *)lparam;
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }

        case WM_CREATE: {
            state = (InfoBoxState *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
            state->bg_brush = CreateSolidBrush(COLOR_INFO_BG);
            state->panel_brush = CreateSolidBrush(COLOR_INFO_PANEL);
            state->title_font = CreateFontA(
                -20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
            );
            state->body_font = CreateFontA(
                -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
            );

            state->ok_btn = CreateWindowExA(
                0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                0, 0, 96, 32,
                hwnd, (HMENU)(INT_PTR)IDOK, (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE), NULL
            );
            if (state->body_font) {
                SendMessageA(state->ok_btn, WM_SETFONT, (WPARAM)state->body_font, TRUE);
            }
            return 0;
        }

        case WM_SIZE: {
            if (state && state->ok_btn) {
                int w = LOWORD(lparam);
                int h = HIWORD(lparam);
                MoveWindow(state->ok_btn, w - 120, h - 50, 96, 30, TRUE);
            }
            return 0;
        }

        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wparam;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_TEXT);
            return (LRESULT)(state ? state->panel_brush : GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wparam, &rc, state ? state->bg_brush : (HBRUSH)GetStockObject(BLACK_BRUSH));
            RECT panel = rc;
            panel.left += 12;
            panel.top += 12;
            panel.right -= 12;
            panel.bottom -= 64;
            FillRect((HDC)wparam, &panel, state ? state->panel_brush : (HBRUSH)GetStockObject(GRAY_BRUSH));
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT title_rc;
            GetClientRect(hwnd, &title_rc);
            title_rc.left = 24;
            title_rc.top = 22;
            title_rc.right -= 24;
            title_rc.bottom = 56;

            HFONT old = (HFONT)SelectObject(hdc, state && state->title_font ? state->title_font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, COLOR_TEXT);
            DrawTextA(hdc, state ? state->title : "", -1, &title_rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            RECT body_rc;
            GetClientRect(hwnd, &body_rc);
            body_rc.left = 28;
            body_rc.top = 64;
            body_rc.right -= 28;
            body_rc.bottom -= 78;

            SelectObject(hdc, state && state->body_font ? state->body_font : GetStockObject(DEFAULT_GUI_FONT));
            SetTextColor(hdc, COLOR_SUBTEXT);
            DrawTextA(hdc, state ? state->message : "", -1, &body_rc, DT_LEFT | DT_TOP | DT_WORDBREAK);

            SelectObject(hdc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_NCDESTROY:
            if (state) {
                if (state->title_font) DeleteObject(state->title_font);
                if (state->body_font) DeleteObject(state->body_font);
                if (state->bg_brush) DeleteObject(state->bg_brush);
                if (state->panel_brush) DeleteObject(state->panel_brush);
                free(state->title);
                free(state->message);
                free(state);
            }
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
            return 0;

        default:
            break;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

static BOOL register_info_box_class(HINSTANCE instance) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = info_box_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "EditorInfoBoxClass";
    return RegisterClassA(&wc) != 0;
}

static void show_skinned_info_box(HWND parent, const char *title, const char *message) {
    InfoBoxState *state = (InfoBoxState *)calloc(1, sizeof(*state));
    if (!state) {
        MessageBoxA(parent, message, title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    state->title = dup_text(title ? title : "Info");
    state->message = dup_text(message ? message : "");
    if (!state->title || !state->message) {
        free(state->title);
        free(state->message);
        free(state);
        MessageBoxA(parent, message, title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    HWND box = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        "EditorInfoBoxClass",
        title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 320,
        parent,
        NULL,
        (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE),
        state
    );
    if (!box) {
        free(state->title);
        free(state->message);
        free(state);
        MessageBoxA(parent, message, title, MB_OK | MB_ICONINFORMATION);
        return;
    }

    RECT pr = {0};
    RECT br = {0};
    GetWindowRect(parent, &pr);
    GetWindowRect(box, &br);
    int x = pr.left + ((pr.right - pr.left) - (br.right - br.left)) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - (br.bottom - br.top)) / 2;
    SetWindowPos(box, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    EnableWindow(parent, FALSE);
    SetForegroundWindow(box);
    SetFocus(box);

    MSG msg;
    while (IsWindow(box) && GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(box, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetActiveWindow(parent);
}

static void free_menu_texts(void) {
    for (int i = 0; i < g_menu_text_count; i++) {
        free(g_menu_texts[i]);
        g_menu_texts[i] = NULL;
    }
    g_menu_text_count = 0;
}

static char *dup_menu_text(const char *text) {
    size_t n = strlen(text) + 1u;
    char *copy = (char *)malloc(n);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    if (g_menu_text_count < MAX_MENU_TEXTS) {
        g_menu_texts[g_menu_text_count++] = copy;
    }
    return copy;
}

static BOOL append_ownerdraw_item(HMENU menu, UINT flags, UINT_PTR item, const char *text) {
    char *stored = dup_menu_text(text);
    if (!stored) {
        return AppendMenuA(menu, flags, item, text);
    }
    return AppendMenuA(menu, flags | MF_OWNERDRAW, item, (LPCSTR)stored);
}

static void ensure_menu_font(void) {
    if (g_menu_font) return;

    NONCLIENTMETRICSA ncm = {0};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_menu_font = CreateFontIndirectA(&ncm.lfMenuFont);
    }
    if (!g_menu_font) {
        g_menu_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
}

static void apply_menu_background_recursive(HMENU menu) {
    if (!g_menu_bg_brush) return;

    MENUINFO mi = {0};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = g_menu_bg_brush;
    SetMenuInfo(menu, &mi);

    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; i++) {
        HMENU sub = GetSubMenu(menu, i);
        if (sub) {
            apply_menu_background_recursive(sub);
        }
    }
}

static void split_menu_text(const char *text, char *left, size_t left_cap, char *right, size_t right_cap) {
    const char *tab = strchr(text, '\t');
    if (!tab) {
        lstrcpynA(left, text, (int)left_cap);
        right[0] = '\0';
        return;
    }

    size_t left_len = (size_t)(tab - text);
    if (left_len >= left_cap) left_len = left_cap - 1u;
    memcpy(left, text, left_len);
    left[left_len] = '\0';
    lstrcpynA(right, tab + 1, (int)right_cap);
}

static void normalize_menu_label(const char *src, char *dst, size_t dst_cap) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] != '\0' && j + 1u < dst_cap; i++) {
        if (src[i] == '&') {
            if (src[i + 1] == '&') {
                dst[j++] = '&';
                i++;
            }
            continue;
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

static void enable_dark_menus(void) {
    HMODULE uxtheme = LoadLibraryA("uxtheme.dll");
    if (!uxtheme) return;

    typedef int (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *FlushMenuThemesFn)(void);

    FARPROC raw_set_mode = GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    FARPROC raw_flush = GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    SetPreferredAppModeFn set_mode = NULL;
    FlushMenuThemesFn flush_menu_themes = NULL;

    if (raw_set_mode) {
        memcpy(&set_mode, &raw_set_mode, sizeof(set_mode));
    }
    if (raw_flush) {
        memcpy(&flush_menu_themes, &raw_flush, sizeof(flush_menu_themes));
    }

    if (set_mode) {
        const int AllowDarkMode = 1;
        set_mode(AllowDarkMode);
        if (flush_menu_themes) {
            flush_menu_themes();
        }
    }

    FreeLibrary(uxtheme);
}

static void load_icons_from_exe(HICON *out_big, HICON *out_small) {
    if (!out_big || !out_small) return;
    *out_big = NULL;
    *out_small = NULL;

    char exe_path[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) == 0) {
        return;
    }

    UINT extracted = ExtractIconExA(exe_path, 0, out_big, out_small, 1);
    if (extracted == 0) {
        *out_big = NULL;
        *out_small = NULL;
    }
}

static void update_window_title(HWND hwnd) {
    char title[MAX_PATH + 64];
    if (g_current_file[0]) {
        wsprintfA(title, "Editor - %s", g_current_file);
    } else {
        lstrcpynA(title, "Editor - Untitled", (int)sizeof(title));
    }
    SetWindowTextA(hwnd, title);
    request_render();
}

static char *get_editor_text(size_t *out_len) {
    int len = GetWindowTextLengthA(g_edit);
    char *buffer = (char *)malloc((size_t)len + 1u);
    if (!buffer) {
        return NULL;
    }
    GetWindowTextA(g_edit, buffer, len + 1);
    if (out_len) {
        *out_len = (size_t)len;
    }
    return buffer;
}

static void apply_dark_title_bar(HWND hwnd) {
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;

    typedef HRESULT (WINAPI *DwmSetWindowAttributeFn)(HWND, DWORD, LPCVOID, DWORD);
    FARPROC raw = GetProcAddress(dwm, "DwmSetWindowAttribute");
    DwmSetWindowAttributeFn set_attr = NULL;
    if (raw) {
        memcpy(&set_attr, &raw, sizeof(set_attr));
    }
    if (set_attr) {
        BOOL enabled = TRUE;
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        set_attr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
    }

    FreeLibrary(dwm);
}

static void save_editor_to_path(HWND hwnd, const char *path) {
    size_t len = 0;
    char *buffer = get_editor_text(&len);
    if (!buffer) {
        MessageBoxA(hwnd, "Out of memory while saving.", "Save Error", MB_OK | MB_ICONERROR);
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buffer);
        MessageBoxA(hwnd, "Could not open file for writing.", "Save Error", MB_OK | MB_ICONERROR);
        return;
    }

    fwrite(buffer, 1, len, f);
    fclose(f);
    free(buffer);

    lstrcpynA(g_current_file, path, MAX_PATH);
    update_window_title(hwnd);
}

static void save_to_output_txt(HWND hwnd) {
    save_editor_to_path(hwnd, g_current_file[0] ? g_current_file : "output.txt");
}

static void show_file_info_prompt(HWND hwnd) {
    size_t len = 0;
    char *buffer = get_editor_text(&len);
    if (!buffer) {
        MessageBoxA(hwnd, "Out of memory while gathering file info.", "File Info", MB_OK | MB_ICONERROR);
        return;
    }

    int lines = 1;
    for (size_t i = 0; i < len; i++) {
        if (buffer[i] == '\n') lines++;
    }
    if (len == 0) lines = 0;

    const char *path = g_current_file[0] ? g_current_file : "(unsaved)";
    char msg[1024];
    wsprintfA(
        msg,
        "File: %s\nCharacters: %u\nApprox. bytes: %u\nLines: %d",
        path,
        (unsigned)len,
        (unsigned)len,
        lines
    );
    show_skinned_info_box(hwnd, "File Info", msg);
    free(buffer);
}

static BOOL load_file_into_editor(HWND hwnd, const char *path) {
    if (!g_edit || !path || path[0] == '\0') {
        log_message("load_file_into_editor: invalid state g_edit=%p path=%s", (void *)g_edit, path ? path : "(null)");
        return FALSE;
    }

    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (file == INVALID_HANDLE_VALUE) {
        log_message("load_file_into_editor: CreateFileA failed path=%s err=%lu", path, (unsigned long)GetLastError());
        return FALSE;
    }

    LARGE_INTEGER file_size = {0};
    if (!GetFileSizeEx(file, &file_size)) {
        log_message("load_file_into_editor: GetFileSizeEx failed path=%s err=%lu", path, (unsigned long)GetLastError());
        CloseHandle(file);
        return FALSE;
    }
    log_message("load_file_into_editor: path=%s size=%lld", path, (long long)file_size.QuadPart);

    if (file_size.QuadPart > (__int64)0x7FFFFFFE) {
        CloseHandle(file);
        log_message("load_file_into_editor: file too large path=%s", path);
        MessageBoxA(hwnd, "File is too large for the editor control.", "Open Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (file_size.QuadPart < 0) {
        log_message("load_file_into_editor: unsupported file size path=%s size=%lld", path, (long long)file_size.QuadPart);
        CloseHandle(file);
        return FALSE;
    }

    size_t size = (size_t)file_size.QuadPart;
    char *buffer = (char *)malloc(size + 1u);
    if (!buffer) {
        log_message("load_file_into_editor: malloc failed path=%s size=%llu", path, (unsigned long long)size);
        CloseHandle(file);
        return FALSE;
    }

    size_t total_read = 0;
    while (total_read < size) {
        DWORD chunk_read = 0;
        size_t remaining = size - total_read;
        DWORD to_read = (remaining > 1024u * 1024u) ? (1024u * 1024u) : (DWORD)remaining;
        if (!ReadFile(file, buffer + total_read, to_read, &chunk_read, NULL)) {
            log_message("load_file_into_editor: ReadFile failed path=%s err=%lu", path, (unsigned long)GetLastError());
            CloseHandle(file);
            free(buffer);
            return FALSE;
        }
        if (chunk_read == 0) {
            break;
        }
        total_read += (size_t)chunk_read;
    }
    CloseHandle(file);

    if (total_read != size) {
        log_message(
            "load_file_into_editor: short read path=%s expected=%llu got=%llu",
            path,
            (unsigned long long)size,
            (unsigned long long)total_read
        );
        free(buffer);
        return FALSE;
    }
    buffer[size] = '\0';

    if (size >= 2 && (unsigned char)buffer[0] == 0xFF && (unsigned char)buffer[1] == 0xFE) {
        size_t utf16_bytes = size - 2u;
        int wchar_count = (int)(utf16_bytes / 2u);
        WCHAR *wide = NULL;
        char *converted = NULL;
        int out_bytes = 0;

        wide = (WCHAR *)malloc(((size_t)wchar_count + 1u) * sizeof(WCHAR));
        if (!wide) {
            free(buffer);
            return FALSE;
        }
        memcpy(wide, buffer + 2, (size_t)wchar_count * sizeof(WCHAR));
        wide[wchar_count] = L'\0';

        out_bytes = WideCharToMultiByte(CP_ACP, 0, wide, wchar_count, NULL, 0, NULL, NULL);
        if (out_bytes <= 0) {
            free(wide);
            free(buffer);
            return FALSE;
        }

        converted = (char *)malloc((size_t)out_bytes + 1u);
        if (!converted) {
            free(wide);
            free(buffer);
            return FALSE;
        }
        if (WideCharToMultiByte(CP_ACP, 0, wide, wchar_count, converted, out_bytes, NULL, NULL) <= 0) {
            free(converted);
            free(wide);
            free(buffer);
            return FALSE;
        }
        converted[out_bytes] = '\0';
        free(wide);
        free(buffer);
        buffer = converted;
        size = (size_t)out_bytes;
        log_message("load_file_into_editor: converted UTF-16LE BOM file path=%s", path);
    } else if (size >= 3 &&
               (unsigned char)buffer[0] == 0xEF &&
               (unsigned char)buffer[1] == 0xBB &&
               (unsigned char)buffer[2] == 0xBF) {
        memmove(buffer, buffer + 3, size - 3u);
        size -= 3u;
        buffer[size] = '\0';
        log_message("load_file_into_editor: stripped UTF-8 BOM path=%s", path);
    }

    for (size_t i = 0; i < size; i++) {
        if (buffer[i] == '\0') {
            buffer[i] = ' ';
        }
    }
    buffer[size] = '\0';

    if (!SendMessageA(g_edit, WM_SETTEXT, 0, (LPARAM)buffer)) {
        log_message("load_file_into_editor: WM_SETTEXT failed path=%s", path);
        free(buffer);
        return FALSE;
    }
    lstrcpynA(g_current_file, path, MAX_PATH);
    update_window_title(hwnd);
    free(buffer);
    log_message("load_file_into_editor: success path=%s bytes=%llu", path, (unsigned long long)size);
    return TRUE;
}

static void open_file_into_editor(HWND hwnd) {
    OPENFILENAMEA ofn = {0};
    char path[MAX_PATH] = {0};

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = "Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) {
        return;
    }

    if (!load_file_into_editor(hwnd, path)) {
        MessageBoxA(hwnd, "Could not open the selected file.", "Open Error", MB_OK | MB_ICONERROR);
    }
}

static void apply_editor_font(const LOGFONTA *lf) {
    HFONT new_font = CreateFontIndirectA(lf);
    if (!new_font) return;

    SendMessageA(g_edit, WM_SETFONT, (WPARAM)new_font, TRUE);
    if (g_font) {
        DeleteObject(g_font);
    }
    g_font = new_font;
}

static void choose_editor_font(HWND hwnd) {
    CHOOSEFONTA cf = {0};
    LOGFONTA lf = g_logfont;

    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;

    if (ChooseFontA(&cf)) {
        g_logfont = lf;
        apply_editor_font(&g_logfont);
    }
}

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_KEYDOWN && wparam == VK_TAB) {
        SendMessageA(hwnd, EM_REPLACESEL, TRUE, (LPARAM)"\t");
        return 0;
    }
    return CallWindowProcA(g_edit_proc, hwnd, msg, wparam, lparam);
}

static DWORD get_edit_style(void) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                  ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL;
    if (!g_word_wrap) {
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    }
    return style;
}

static void create_editor_control(HWND hwnd, HINSTANCE instance, const char *initial_text) {
    RECT rc;
    get_editor_rect(hwnd, &rc);

    g_edit = CreateWindowExA(
        0,
        "EDIT",
        initial_text ? initial_text : "",
        get_edit_style(),
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hwnd,
        (HMENU)(INT_PTR)ID_EDIT,
        instance,
        NULL
    );

    // Raise the default multiline EDIT limit (~32KB) so full files can load.
    SendMessageA(g_edit, EM_LIMITTEXT, 0x7FFFFFFE, 0);
    g_edit_proc = (WNDPROC)SetWindowLongPtrA(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    apply_editor_font(&g_logfont);
    SendMessageA(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
    SendMessageA(g_edit, EM_SETREADONLY, g_read_only, 0);
}

static void recreate_editor_control(HWND hwnd) {
    char *text = get_editor_text(NULL);
    DWORD sel_start = 0;
    DWORD sel_end = 0;
    SendMessageA(g_edit, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);

    DestroyWindow(g_edit);
    create_editor_control(hwnd, (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE), text ? text : "");

    if (text) {
        free(text);
    }
    SendMessageA(g_edit, EM_SETSEL, sel_start, sel_end);
}

static HMENU build_menu(void) {
    HMENU main_menu = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU edit_menu = CreatePopupMenu();
    HMENU view_menu = CreatePopupMenu();
    HMENU format_menu = CreatePopupMenu();
    HMENU help_menu = CreatePopupMenu();

    append_ownerdraw_item(file_menu, MF_STRING, ID_FILE_NEW, "&New\tCtrl+N");
    append_ownerdraw_item(file_menu, MF_STRING, ID_FILE_OPEN, "&Open...\tCtrl+O");
    append_ownerdraw_item(file_menu, MF_STRING, ID_FILE_SAVE, "&Save\tCtrl+S");
    append_ownerdraw_item(file_menu, MF_STRING, ID_FILE_INFO, "File &Info");
    AppendMenuA(file_menu, MF_SEPARATOR, 0, NULL);
    append_ownerdraw_item(file_menu, MF_STRING, ID_FILE_EXIT, "E&xit\tCtrl+Q");
    append_ownerdraw_item(main_menu, MF_POPUP, (UINT_PTR)file_menu, "&File");

    append_ownerdraw_item(edit_menu, MF_STRING, ID_EDIT_UNDO, "&Undo\tCtrl+Z");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    append_ownerdraw_item(edit_menu, MF_STRING, ID_EDIT_CUT, "Cu&t\tCtrl+X");
    append_ownerdraw_item(edit_menu, MF_STRING, ID_EDIT_COPY, "&Copy\tCtrl+C");
    append_ownerdraw_item(edit_menu, MF_STRING, ID_EDIT_PASTE, "&Paste\tCtrl+V");
    append_ownerdraw_item(edit_menu, MF_STRING, ID_EDIT_DELETE, "&Delete\tDel");
    AppendMenuA(edit_menu, MF_SEPARATOR, 0, NULL);
    append_ownerdraw_item(edit_menu, MF_STRING, ID_EDIT_SELECT_ALL, "Select &All\tCtrl+A");
    append_ownerdraw_item(main_menu, MF_POPUP, (UINT_PTR)edit_menu, "&Edit");

    append_ownerdraw_item(view_menu, MF_STRING, ID_VIEW_READ_ONLY, "&Read Only");
    append_ownerdraw_item(view_menu, MF_STRING, ID_VIEW_WORD_WRAP, "&Word Wrap");
    append_ownerdraw_item(view_menu, MF_STRING, ID_VIEW_ALWAYS_ON_TOP, "Always on &Top");
    append_ownerdraw_item(main_menu, MF_POPUP, (UINT_PTR)view_menu, "&View");

    append_ownerdraw_item(format_menu, MF_STRING, ID_FORMAT_FONT, "&Font...\tCtrl+Shift+F");
    append_ownerdraw_item(main_menu, MF_POPUP, (UINT_PTR)format_menu, "F&ormat");

    append_ownerdraw_item(help_menu, MF_STRING, ID_HELP_ABOUT, "&About");
    append_ownerdraw_item(main_menu, MF_POPUP, (UINT_PTR)help_menu, "&Help");

    apply_menu_background_recursive(main_menu);

    return main_menu;
}

static const char *next_cmd_token(const char *p, char *out, size_t out_cap) {
    size_t len = 0;
    if (!p || !out || out_cap == 0) return NULL;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') {
        out[0] = '\0';
        return NULL;
    }

    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (len + 1u < out_cap) out[len++] = *p;
            p++;
        }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (len + 1u < out_cap) out[len++] = *p;
            p++;
        }
    }

    out[len] = '\0';
    return p;
}

static void apply_launch_parameters(const char *cmdline) {
    const char *p = cmdline;
    char token[MAX_PATH];

    while ((p = next_cmd_token(p, token, sizeof(token))) != NULL) {
        if (token[0] == '\0') {
            continue;
        }

        if (lstrcmpiA(token, "--read-only") == 0 || lstrcmpiA(token, "-r") == 0) {
            g_read_only = TRUE;
            continue;
        }
        if (lstrcmpiA(token, "--word-wrap") == 0 || lstrcmpiA(token, "-w") == 0) {
            g_word_wrap = TRUE;
            continue;
        }
        if (lstrcmpiA(token, "--topmost") == 0 || lstrcmpiA(token, "-t") == 0) {
            g_always_on_top = TRUE;
            continue;
        }

        if (token[0] != '-' && g_launch_file[0] == '\0') {
            lstrcpynA(g_launch_file, token, MAX_PATH);
        }
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE:
            g_bg_brush = CreateSolidBrush(COLOR_BG);
            g_header_brush = CreateSolidBrush(COLOR_HEADER_BG);
            g_panel_brush = CreateSolidBrush(COLOR_PANEL_BG);
            g_editor_brush = CreateSolidBrush(COLOR_EDITOR_BG);
            g_frame_pen = CreatePen(PS_SOLID, 1, COLOR_ACCENT);
            g_menu_bg_brush = CreateSolidBrush(COLOR_MENU_BG);
            g_menu_hot_brush = CreateSolidBrush(COLOR_MENU_HOT);
            ensure_menu_font();
            g_header_font = CreateFontA(
                -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI"
            );
            g_logfont.lfHeight = -18;
            g_logfont.lfWeight = FW_NORMAL;
            g_logfont.lfQuality = CLEARTYPE_QUALITY;
            g_logfont.lfCharSet = DEFAULT_CHARSET;
            g_logfont.lfOutPrecision = OUT_DEFAULT_PRECIS;
            g_logfont.lfClipPrecision = CLIP_DEFAULT_PRECIS;
            g_logfont.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
            lstrcpynA(g_logfont.lfFaceName, "Consolas", LF_FACESIZE);
            create_editor_control(hwnd, ((LPCREATESTRUCTA)lparam)->hInstance, "");
            update_window_title(hwnd);
            apply_dark_title_bar(hwnd);
            if (!d2d_ensure_factory()) {
                start_render_thread(hwnd);
            }
            if (g_launch_file[0]) {
                if (!load_file_into_editor(hwnd, g_launch_file)) {
                    MessageBoxA(hwnd, "Could not open file passed via launch parameters.", "Open Error", MB_OK | MB_ICONERROR);
                }
            }
            if (g_always_on_top) {
                SetWindowPos(
                    hwnd,
                    HWND_TOPMOST,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE
                );
            }
            if (GetMenu(hwnd)) {
                CheckMenuItem(
                    GetMenu(hwnd),
                    ID_VIEW_READ_ONLY,
                    MF_BYCOMMAND | (g_read_only ? MF_CHECKED : MF_UNCHECKED)
                );
                CheckMenuItem(
                    GetMenu(hwnd),
                    ID_VIEW_WORD_WRAP,
                    MF_BYCOMMAND | (g_word_wrap ? MF_CHECKED : MF_UNCHECKED)
                );
                CheckMenuItem(
                    GetMenu(hwnd),
                    ID_VIEW_ALWAYS_ON_TOP,
                    MF_BYCOMMAND | (g_always_on_top ? MF_CHECKED : MF_UNCHECKED)
                );
            }
            if (GetMenu(hwnd)) {
                apply_menu_background_recursive(GetMenu(hwnd));
            }
            request_render();
            return 0;

        case WM_SIZE:
            apply_skin_layout(hwnd);
            d2d_resize(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lparam;
            mmi->ptMinTrackSize.x = WINDOW_MIN_W;
            mmi->ptMinTrackSize.y = WINDOW_MIN_H;
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wparam;
            SetTextColor(hdc, COLOR_TEXT);
            SetBkColor(hdc, COLOR_EDITOR_BG);
            return (LRESULT)g_editor_brush;
        }

        case WM_ERASEBKGND: {
            return 1;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HBITMAP frame = NULL;
            int fw = 0;
            int fh = 0;
            RECT rc;
            int width;
            int height;
            const char *path = g_current_file[0] ? g_current_file : "Untitled";
            GetClientRect(hwnd, &rc);
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;

            if (d2d_draw_chrome(hwnd)) {
                draw_header_text(hdc, width, get_skin_header_h(hwnd), path);
                EndPaint(hwnd, &ps);
                return 0;
            }

            if (g_render_lock_ready) {
                EnterCriticalSection(&g_render_lock);
                frame = g_render_frame;
                fw = g_render_frame_w;
                fh = g_render_frame_h;
                LeaveCriticalSection(&g_render_lock);
            }

            if (frame && fw == (rc.right - rc.left) && fh == (rc.bottom - rc.top)) {
                HDC mem = CreateCompatibleDC(hdc);
                if (mem) {
                    HBITMAP old = (HBITMAP)SelectObject(mem, frame);
                    BitBlt(hdc, 0, 0, fw, fh, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                } else {
                    render_chrome(hdc, width, height, path);
                }
            } else {
                render_chrome(hdc, width, height, path);
                request_render();
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_APP_RENDER_READY:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;

        case WM_SETTINGCHANGE:
            enable_dark_menus();
            if (GetMenu(hwnd)) {
                apply_menu_background_recursive(GetMenu(hwnd));
            }
            DrawMenuBar(hwnd);
            request_render();
            return 0;

        case WM_MEASUREITEM: {
            LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lparam;
            if (!mis || mis->CtlType != ODT_MENU) {
                break;
            }
            BOOL is_menu_bar_item = ((HMENU)(UINT_PTR)mis->CtlID == GetMenu(hwnd));

            const char *text = (const char *)mis->itemData;
            if (!text) {
                mis->itemWidth = 0;
                mis->itemHeight = 8;
                return TRUE;
            }

            char left[256];
            char right[128];
            char left_draw[256];
            char right_draw[128];
            split_menu_text(text, left, sizeof(left), right, sizeof(right));
            normalize_menu_label(left, left_draw, sizeof(left_draw));
            normalize_menu_label(right, right_draw, sizeof(right_draw));

            HDC hdc = GetDC(hwnd);
            ensure_menu_font();
            HFONT old_font = (HFONT)SelectObject(hdc, g_menu_font);
            SIZE sz_left = {0};
            SIZE sz_right = {0};
            GetTextExtentPoint32A(hdc, left_draw, lstrlenA(left_draw), &sz_left);
            if (right_draw[0]) {
                GetTextExtentPoint32A(hdc, right_draw, lstrlenA(right_draw), &sz_right);
            }
            SelectObject(hdc, old_font);
            ReleaseDC(hwnd, hdc);

            mis->itemHeight = (UINT)((sz_left.cy + 8) < 24 ? 24 : (sz_left.cy + 8));
            if (is_menu_bar_item) {
                mis->itemWidth = (UINT)(sz_left.cx + 18);
            } else {
                UINT width = (UINT)(sz_left.cx + 40);
                if (right_draw[0]) {
                    width += (UINT)(sz_right.cx + 24);
                }
                mis->itemWidth = width;
            }
            return TRUE;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lparam;
            if (!dis || dis->CtlType != ODT_MENU) {
                break;
            }
            BOOL is_menu_bar_item = ((HMENU)dis->hwndItem == GetMenu(hwnd));

            const char *text = (const char *)dis->itemData;
            if (!text) return TRUE;

            char left[256];
            char right[128];
            char left_draw[256];
            char right_draw[128];
            split_menu_text(text, left, sizeof(left), right, sizeof(right));
            normalize_menu_label(left, left_draw, sizeof(left_draw));
            normalize_menu_label(right, right_draw, sizeof(right_draw));

            BOOL selected = (dis->itemState & ODS_SELECTED) != 0;
            BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;
            BOOL checked = (dis->itemState & ODS_CHECKED) != 0;

            RECT rc = dis->rcItem;
            FillRect(dis->hDC, &rc, selected ? g_menu_hot_brush : g_menu_bg_brush);

            ensure_menu_font();
            HFONT old_font = (HFONT)SelectObject(dis->hDC, g_menu_font);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, disabled ? COLOR_MENU_TEXT_DISABLED : COLOR_MENU_TEXT);

            if (!is_menu_bar_item && checked) {
                RECT mark = rc;
                mark.left += 8;
                mark.right = mark.left + 14;
                mark.top += 5;
                mark.bottom -= 5;
                DrawFrameControl(dis->hDC, &mark, DFC_MENU, DFCS_MENUCHECK);
            }

            int text_x = is_menu_bar_item ? (rc.left + 8) : (rc.left + 28);
            TextOutA(dis->hDC, text_x, rc.top + 4, left_draw, lstrlenA(left_draw));

            if (right_draw[0] && !is_menu_bar_item) {
                SIZE sz_right = {0};
                GetTextExtentPoint32A(dis->hDC, right_draw, lstrlenA(right_draw), &sz_right);
                TextOutA(dis->hDC, rc.right - sz_right.cx - 12, rc.top + 4, right_draw, lstrlenA(right_draw));
            }

            SelectObject(dis->hDC, old_font);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case ID_FILE_NEW:
                    SetWindowTextA(g_edit, "");
                    g_current_file[0] = '\0';
                    update_window_title(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                case ID_FILE_OPEN:
                    open_file_into_editor(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                case ID_FILE_SAVE:
                    save_to_output_txt(hwnd);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                case ID_FILE_INFO:
                    show_file_info_prompt(hwnd);
                    return 0;
                case ID_FILE_EXIT:
                    PostMessage(hwnd, WM_CLOSE, 0, 0);
                    return 0;
                case ID_EDIT_UNDO:
                    if (SendMessageA(g_edit, EM_CANUNDO, 0, 0)) {
                        SendMessageA(g_edit, WM_UNDO, 0, 0);
                    }
                    return 0;
                case ID_EDIT_CUT:
                    SendMessageA(g_edit, WM_CUT, 0, 0);
                    return 0;
                case ID_EDIT_COPY:
                    SendMessageA(g_edit, WM_COPY, 0, 0);
                    return 0;
                case ID_EDIT_PASTE:
                    SendMessageA(g_edit, WM_PASTE, 0, 0);
                    return 0;
                case ID_EDIT_DELETE:
                    SendMessageA(g_edit, WM_CLEAR, 0, 0);
                    return 0;
                case ID_EDIT_SELECT_ALL:
                    SendMessageA(g_edit, EM_SETSEL, 0, -1);
                    return 0;
                case ID_VIEW_READ_ONLY: {
                    HMENU menu = GetMenu(hwnd);
                    g_read_only = !g_read_only;
                    SendMessageA(g_edit, EM_SETREADONLY, g_read_only, 0);
                    CheckMenuItem(
                        menu,
                        ID_VIEW_READ_ONLY,
                        MF_BYCOMMAND | (g_read_only ? MF_CHECKED : MF_UNCHECKED)
                    );
                    return 0;
                }
                case ID_VIEW_ALWAYS_ON_TOP: {
                    HMENU menu = GetMenu(hwnd);
                    g_always_on_top = !g_always_on_top;
                    SetWindowPos(
                        hwnd,
                        g_always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                        0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE
                    );
                    CheckMenuItem(
                        menu,
                        ID_VIEW_ALWAYS_ON_TOP,
                        MF_BYCOMMAND | (g_always_on_top ? MF_CHECKED : MF_UNCHECKED)
                    );
                    return 0;
                }
                case ID_VIEW_WORD_WRAP: {
                    HMENU menu = GetMenu(hwnd);
                    g_word_wrap = !g_word_wrap;
                    CheckMenuItem(
                        menu,
                        ID_VIEW_WORD_WRAP,
                        MF_BYCOMMAND | (g_word_wrap ? MF_CHECKED : MF_UNCHECKED)
                    );
                    recreate_editor_control(hwnd);
                    return 0;
                }
                case ID_HELP_ABOUT:
                    show_skinned_info_box(
                        hwnd,
                        "About Editor",
                        "Editor\nBenno111\n\nShortcuts:\nCtrl+N, Ctrl+O, Ctrl+S, Ctrl+Q, Ctrl+Shift+F\nFeatures: Word Wrap, File Info, Tab Insert\nMenu: Custom Dark Menu Bar"
                    );
                    return 0;
                case ID_FORMAT_FONT:
                    choose_editor_font(hwnd);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_DESTROY:
            stop_render_thread();
            d2d_release_target();
            if (g_d2d_factory) {
                ID2D1Factory_Release(g_d2d_factory);
                g_d2d_factory = NULL;
            }
            if (g_font) {
                DeleteObject(g_font);
                g_font = NULL;
            }
            if (g_bg_brush) {
                DeleteObject(g_bg_brush);
                g_bg_brush = NULL;
            }
            if (g_editor_brush) {
                DeleteObject(g_editor_brush);
                g_editor_brush = NULL;
            }
            if (g_header_brush) {
                DeleteObject(g_header_brush);
                g_header_brush = NULL;
            }
            if (g_panel_brush) {
                DeleteObject(g_panel_brush);
                g_panel_brush = NULL;
            }
            if (g_frame_pen) {
                DeleteObject(g_frame_pen);
                g_frame_pen = NULL;
            }
            if (g_menu_bg_brush) {
                DeleteObject(g_menu_bg_brush);
                g_menu_bg_brush = NULL;
            }
            if (g_menu_hot_brush) {
                DeleteObject(g_menu_hot_brush);
                g_menu_hot_brush = NULL;
            }
            if (g_header_font) {
                DeleteObject(g_header_font);
                g_header_font = NULL;
            }
            if (g_menu_font && g_menu_font != GetStockObject(DEFAULT_GUI_FONT)) {
                DeleteObject(g_menu_font);
            }
            g_menu_font = NULL;
            free_menu_texts();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev;
    init_logging();
    log_message("WinMain start cmd=%s", cmd ? cmd : "");

    HICON app_icon = NULL;
    HICON small_icon = NULL;
    load_icons_from_exe(&app_icon, &small_icon);
    if (!app_icon) {
        app_icon = LoadIconA(NULL, IDI_APPLICATION);
    }
    if (!small_icon) {
        small_icon = app_icon;
    }
    apply_launch_parameters(cmd);

    const char *class_name = "TinyCEditorWindow";
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hIcon = app_icon;
    wc.hIconSm = small_icon;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;

    if (!RegisterClassExA(&wc)) {
        log_message("RegisterClassExA failed err=%lu", (unsigned long)GetLastError());
        return 1;
    }
    register_info_box_class(instance);

    enable_dark_menus();

    HWND hwnd = CreateWindowExA(
        0,
        class_name,
        "Editor",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        NULL,
        build_menu(),
        instance,
        NULL
    );

    if (!hwnd) {
        log_message("CreateWindowExA failed err=%lu", (unsigned long)GetLastError());
        return 1;
    }
    log_message("CreateWindowExA success hwnd=%p", (void *)hwnd);

    ACCEL accels[] = {
        {FVIRTKEY | FCONTROL, 'N', ID_FILE_NEW},
        {FVIRTKEY | FCONTROL, 'O', ID_FILE_OPEN},
        {FVIRTKEY | FCONTROL, 'S', ID_FILE_SAVE},
        {FVIRTKEY | FCONTROL, 'Q', ID_FILE_EXIT},
        {FVIRTKEY | FCONTROL, 'Z', ID_EDIT_UNDO},
        {FVIRTKEY | FCONTROL, 'X', ID_EDIT_CUT},
        {FVIRTKEY | FCONTROL, 'C', ID_EDIT_COPY},
        {FVIRTKEY | FCONTROL, 'V', ID_EDIT_PASTE},
        {FVIRTKEY | FCONTROL, 'A', ID_EDIT_SELECT_ALL},
        {FVIRTKEY | FCONTROL | FSHIFT, 'F', ID_FORMAT_FONT}
    };
    HACCEL accel_table = CreateAcceleratorTableA(accels, (int)(sizeof(accels) / sizeof(accels[0])));

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorA(hwnd, accel_table, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (accel_table) {
        DestroyAcceleratorTable(accel_table);
    }

    log_message("WinMain exit code=%ld", (long)msg.wParam);
    close_logging();
    return (int)msg.wParam;
}
