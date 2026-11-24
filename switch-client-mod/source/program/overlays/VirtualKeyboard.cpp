#include "VirtualKeyboard.hpp"
#include "lib.hpp"
#include "imgui/imgui.h"
#include <string>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace VirtualKeyboard {

static bool   s_isOpen          = false;
static float  s_kbAnim          = 0.0f;
static char*  s_targetBuf       = nullptr;
static size_t s_bufSize         = 0;
static std::string s_submitLabel = "SUBMIT";
static void (*s_onSubmitCb)()   = nullptr;

static bool s_capsLock          = false;
static int  s_skipCloseFrames   = 0;
static int  s_cursorPos         = 0;

static void InsertTextAtCursor(const char* text);
static void BackspaceAtCursor();
static void ClearAtCursor();

/* pad navigation state (only MpUi drives this) */
static bool s_padMode           = false;
static int  s_focusR            = 1;   /* focused key row (default: 'q') */
static int  s_focusC            = 0;
struct KbKey { ImVec2 a, b; const char* label; int act; };
static KbKey s_keys[5][16];            /* act: 0 insert 1 bs 2 clr 3 caps
                                        *      4 close 5 < 6 > 7 space 8 ok */
static const int kCols[5] = {16, 14, 14, 12, 5};

void SetPadMode(bool on) {
    if (on && !s_padMode) { s_focusR = 1; s_focusC = 0; }
    s_padMode = on;
}

static void ActivateFocused() {
    if (s_focusR < 0 || s_focusR > 4) return;
    if (s_focusC < 0 || s_focusC >= kCols[s_focusR]) return;
    const KbKey& k = s_keys[s_focusR][s_focusC];
    switch (k.act) {
        case 0: InsertTextAtCursor(k.label); break;
        case 1: BackspaceAtCursor(); break;
        case 2: ClearAtCursor(); break;
        case 3: s_capsLock = !s_capsLock; break;
        case 4: s_isOpen = false; break;
        case 5:
            if (s_cursorPos > 0) {
                int p = s_cursorPos - 1;
                while (p > 0 && (static_cast<uint8_t>(s_targetBuf[p]) & 0xC0) == 0x80) p--;
                s_cursorPos = p;
            }
            break;
        case 6:
            if (s_targetBuf) {
                int curLen = static_cast<int>(strlen(s_targetBuf));
                if (s_cursorPos < curLen) {
                    int p = s_cursorPos + 1;
                    while (p < curLen && (static_cast<uint8_t>(s_targetBuf[p]) & 0xC0) == 0x80) p++;
                    s_cursorPos = p;
                }
            }
            break;
        case 7: InsertTextAtCursor(" "); break;
        case 8:
            if (s_onSubmitCb) s_onSubmitCb();
            s_isOpen = false;
            break;
    }
}

void PadInput(uint64_t edges, int moveX, int moveY) {
    if (!s_padMode || !s_isOpen) return;
    constexpr uint64_t A_ = 1ULL << 0, B_ = 1ULL << 1, X_ = 1ULL << 2,
                       Y_ = 1ULL << 3, L_ = 1ULL << 6, R_ = 1ULL << 7,
                       Plus_ = 1ULL << 10;
    if (moveX) {
        float cx = (s_keys[s_focusR][s_focusC].a.x + s_keys[s_focusR][s_focusC].b.x) * 0.5f;
        int best = -1; float bd = 1e30f;
        for (int i = 0; i < kCols[s_focusR]; i++) {
            float ix = (s_keys[s_focusR][i].a.x + s_keys[s_focusR][i].b.x) * 0.5f;
            float d = moveX > 0 ? ix - cx : cx - ix;
            if (d > 1.0f && d < bd) { bd = d; best = i; }
        }
        if (best < 0) best = moveX > 0 ? kCols[s_focusR] - 1 : 0;
        s_focusC = best;
    }
    if (moveY) {
        int r = s_focusR + moveY;
        if (r < 0) r = 0;
        if (r > 4) r = 4;
        float cx = (s_keys[s_focusR][s_focusC].a.x + s_keys[s_focusR][s_focusC].b.x) * 0.5f;
        int best = 0; float bd = 1e30f;
        for (int i = 0; i < kCols[r]; i++) {
            float ix = (s_keys[r][i].a.x + s_keys[r][i].b.x) * 0.5f;
            float d = std::fabs(ix - cx);
            if (d < bd) { bd = d; best = i; }
        }
        s_focusR = r; s_focusC = best;
    }
    if (edges & A_) ActivateFocused();
    if (edges & B_) BackspaceAtCursor();
    if (edges & Y_) InsertTextAtCursor(" ");
    if (edges & X_) s_capsLock = !s_capsLock;
    if (edges & L_) {
        if (s_cursorPos > 0) {
            int p = s_cursorPos - 1;
            while (p > 0 && (static_cast<uint8_t>(s_targetBuf[p]) & 0xC0) == 0x80) p--;
            s_cursorPos = p;
        }
    }
    if (edges & R_) {
        if (s_targetBuf) {
            int curLen = static_cast<int>(strlen(s_targetBuf));
            if (s_cursorPos < curLen) {
                int p = s_cursorPos + 1;
                while (p < curLen && (static_cast<uint8_t>(s_targetBuf[p]) & 0xC0) == 0x80) p++;
                s_cursorPos = p;
            }
        }
    }
    if (edges & Plus_) {
        if (s_onSubmitCb) s_onSubmitCb();
        s_isOpen = false;
    }
}

std::string ShowSwitchKeyboard(const char* guideText, const char* initialText) {
    return initialText ? std::string(initialText) : "";
}

bool IsOpen() {
    return s_isOpen;
}

int GetCursorPos() {
    return s_cursorPos;
}

bool IsVisible() {
    return s_isOpen || (s_kbAnim > 0.005f);
}

bool IsOpenFor(const char* target_buf) {
    return s_isOpen && s_targetBuf == target_buf;
}

void Open(char* target_buf, size_t buf_size, const char* submit_label, void (*on_submit_cb)()) {
    s_targetBuf       = target_buf;
    s_bufSize         = buf_size;
    s_submitLabel     = submit_label ? submit_label : "SUBMIT";
    s_onSubmitCb      = on_submit_cb;
    s_isOpen          = true;
    s_skipCloseFrames = 3;

    if (target_buf) {
        s_cursorPos = static_cast<int>(strlen(target_buf));
    } else {
        s_cursorPos = 0;
    }
}

void Close() {
    s_isOpen = false;
}

bool ShouldFocusTarget() {
    return false;
}

int InputTextCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (data->CursorPos != s_cursorPos && s_cursorPos >= 0 && s_cursorPos <= data->BufTextLen) {
            data->CursorPos = s_cursorPos;
            data->SelectionStart = s_cursorPos;
            data->SelectionEnd = s_cursorPos;
        } else {
            s_cursorPos = data->CursorPos;
        }
    }
    return 0;
}

static void InsertTextAtCursor(const char* text) {
    if (!s_targetBuf || s_bufSize == 0) return;
    int curLen = static_cast<int>(strlen(s_targetBuf));
    int addLen = static_cast<int>(strlen(text));
    if (curLen + addLen >= static_cast<int>(s_bufSize)) return;

    if (s_cursorPos < 0) s_cursorPos = 0;
    if (s_cursorPos > curLen) s_cursorPos = curLen;

    memmove(s_targetBuf + s_cursorPos + addLen, s_targetBuf + s_cursorPos, curLen - s_cursorPos + 1);
    memcpy(s_targetBuf + s_cursorPos, text, addLen);

    s_cursorPos += addLen;
}

static void BackspaceAtCursor() {
    if (!s_targetBuf || s_bufSize == 0) return;
    int curLen = static_cast<int>(strlen(s_targetBuf));
    if (curLen == 0 || s_cursorPos <= 0) return;

    if (s_cursorPos > curLen) s_cursorPos = curLen;

    int delStart = s_cursorPos - 1;
    while (delStart > 0 && (static_cast<uint8_t>(s_targetBuf[delStart]) & 0xC0) == 0x80) {
        delStart--;
    }

    memmove(s_targetBuf + delStart, s_targetBuf + s_cursorPos, curLen - s_cursorPos + 1);
    s_cursorPos = delStart;
}

static void ClearAtCursor() {
    if (!s_targetBuf || s_bufSize == 0) return;
    s_targetBuf[0] = '\0';
    s_cursorPos = 0;
}

void Draw() {
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    float s = io.DisplaySize.y / 720.0f;

    if (s_skipCloseFrames > 0) {
        s_skipCloseFrames--;
    } else if (s_isOpen && ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
        s_isOpen = false;
    }

    float targetAnim = s_isOpen ? 1.0f : 0.0f;
    s_kbAnim += (targetAnim - s_kbAnim) * std::min(dt * 15.0f, 1.0f);

    if (s_kbAnim <= 0.005f) return;

    const float kbWidth  = 1180.0f * s;
    const float kbHeight = 250.0f * s;
    const float startX   = (io.DisplaySize.x - kbWidth) * 0.5f;
    const float startY   = io.DisplaySize.y - (kbHeight + 10.0f * s) * s_kbAnim;

    ImGui::SetNextWindowPos(ImVec2(startX, startY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kbWidth, kbHeight), ImGuiCond_Always);

    ImGuiWindowFlags kbFlags = ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoResize |
                               ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoSavedSettings |
                               ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoFocusOnAppearing |
                               ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("##VirtualKeyboardOverlay", nullptr, kbFlags)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRectFullScreen();

        ImU32 colBg = IM_COL32(18, 22, 26, (int)(250 * s_kbAnim));
        ImU32 colBd = IM_COL32(255, 255, 255, (int)(45 * s_kbAnim));
        draw->AddRectFilled(ImVec2(startX, startY), ImVec2(startX + kbWidth, startY + kbHeight), colBg, 10.0f * s);
        draw->AddRect(ImVec2(startX, startY), ImVec2(startX + kbWidth, startY + kbHeight), colBd, 10.0f * s, 0, 1.2f);

        ImFont* font = ImGui::GetFont();
        float baseFontSz = font->FontSize;

        const float btnH = 40.0f * s;
        const float gap  = 4.0f * s;

        bool isMouseClicked = ImGui::IsMouseClicked(0);
        ImVec2 mousePos = io.MousePos;

        auto DrawPassiveButton = [&](const char* label, float x, float y, float w, float h, ImU32 colorBase, ImU32 colorHov, ImU32 colorPrs, ImU32 colorTxt) -> bool {
            ImVec2 pMin(x, y);
            ImVec2 pMax(x + w, y + h);
            bool isHovered = (mousePos.x >= pMin.x && mousePos.x <= pMax.x && mousePos.y >= pMin.y && mousePos.y <= pMax.y);
            bool isDown = isHovered && ImGui::IsMouseDown(0);

            ImU32 bg = isDown ? colorPrs : (isHovered ? colorHov : colorBase);
            draw->AddRectFilled(pMin, pMax, bg, 4.0f * s);

            float fSize = baseFontSz * (strlen(label) <= 2 ? 1.30f : 1.10f);
            ImVec2 txtSz = font->CalcTextSizeA(fSize, FLT_MAX, 0.0f, label);
            ImVec2 txtPos(pMin.x + (w - txtSz.x) * 0.5f, pMin.y + (h - txtSz.y) * 0.5f);
            draw->AddText(font, fSize, txtPos, colorTxt, label);

            return isHovered && isMouseClicked;
        };

        ImU32 btnBg  = IM_COL32(32, 38, 46, (int)(235 * s_kbAnim));
        ImU32 btnHov = IM_COL32(55, 70, 90, (int)(255 * s_kbAnim));
        ImU32 btnPrs = IM_COL32(210, 216, 222, (int)(255 * s_kbAnim));
        ImU32 txtCol = IM_COL32(235, 240, 245, (int)(250 * s_kbAnim));
        ImU32 txtPrs = IM_COL32(35, 42, 48, (int)(255 * s_kbAnim));

        /* records a key's rect for pad navigation, then draws it */
        auto Key = [&](int row, int col, int act, const char* label,
                       float x, float y, float w, float h,
                       ImU32 c0, ImU32 c1, ImU32 c2, ImU32 ct) -> bool {
            if (row >= 0 && row < 5 && col >= 0 && col < 16)
                s_keys[row][col] = {ImVec2(x, y), ImVec2(x + w, y + h), label, act};
            return DrawPassiveButton(label, x, y, w, h, c0, c1, c2, ct);
        };

        float curY = startY + 8.0f * s;

        const char* r0[] = {"1","2","3","4","5","6","7","8","9","0","-","_","!","&","?"};
        float r0_btnW = (kbWidth - 16.0f * s - (15 * gap) - 75.0f * s) / 15.0f;
        float curX = startX + 8.0f * s;
        for (int i = 0; i < 15; i++) {
            if (Key(0, i, 0, r0[i], curX, curY, r0_btnW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
                InsertTextAtCursor(r0[i]);
            }
            curX += r0_btnW + gap;
        }
        if (Key(0, 15, 1, "<-", curX, curY, 75.0f * s, btnH, btnBg, btnHov, btnPrs, txtCol)) {
            BackspaceAtCursor();
        }

        curY += btnH + gap;
        curX = startX + 8.0f * s;

        const char* r1_upper[] = {"Q","W","E","R","T","Y","U","I","O","P","(",")","/","'"};
        const char* r1_lower[] = {"q","w","e","r","t","y","u","i","o","p","(",")","/","'"};
        const char** r1 = s_capsLock ? r1_upper : r1_lower;
        float r1_btnW = (kbWidth - 16.0f * s - (13 * gap)) / 14.0f;
        for (int i = 0; i < 14; i++) {
            if (Key(1, i, 0, r1[i], curX, curY, r1_btnW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
                InsertTextAtCursor(r1[i]);
            }
            curX += r1_btnW + gap;
        }

        curY += btnH + gap;
        curX = startX + 8.0f * s;

        float capsBtnW = 65.0f * s;
        float clrBtnW  = 75.0f * s;
        ImU32 capsBg  = s_capsLock ? IM_COL32(0, 204, 166, (int)(245 * s_kbAnim)) : btnBg;
        ImU32 capsTxt = s_capsLock ? IM_COL32(20, 25, 30, (int)(255 * s_kbAnim))  : txtCol;
        if (Key(2, 0, 3, "CAPS", curX, curY, capsBtnW, btnH, capsBg, btnHov, btnPrs, capsTxt)) {
            s_capsLock = !s_capsLock;
        }
        curX += capsBtnW + gap;

        const char* r2_upper[] = {"A","S","D","F","G","H","J","K","L",":",";","~"};
        const char* r2_lower[] = {"a","s","d","f","g","h","j","k","l",":",";","~"};
        const char** r2 = s_capsLock ? r2_upper : r2_lower;
        float r2_btnW = (kbWidth - 16.0f * s - (13 * gap) - capsBtnW - clrBtnW) / 12.0f;
        for (int i = 0; i < 12; i++) {
            if (Key(2, i + 1, 0, r2[i], curX, curY, r2_btnW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
                InsertTextAtCursor(r2[i]);
            }
            curX += r2_btnW + gap;
        }
        if (Key(2, 13, 2, "CLR", curX, curY, clrBtnW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
            ClearAtCursor();
        }

        curY += btnH + gap;
        curX = startX + 8.0f * s;

        const char* r3_upper[] = {"Z","X","C","V","B","N","M",",",".","\"","+","@"};
        const char* r3_lower[] = {"z","x","c","v","b","n","m",",",".","\"","+","@"};
        const char** r3 = s_capsLock ? r3_upper : r3_lower;
        float r3_btnW = (kbWidth - 16.0f * s - (11 * gap)) / 12.0f;
        for (int i = 0; i < 12; i++) {
            if (Key(3, i, 0, r3[i], curX, curY, r3_btnW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
                InsertTextAtCursor(r3[i]);
            }
            curX += r3_btnW + gap;
        }

        curY += btnH + gap;
        curX = startX + 8.0f * s;

        float closeW  = 65.0f * s;
        float arrowW  = 50.0f * s;
        float jumpW   = 200.0f * s;
        float spaceW  = kbWidth - 16.0f * s - (4 * gap) - closeW - (arrowW * 2.0f) - jumpW;

        if (Key(4, 0, 4, "CLOSE", curX, curY, closeW, btnH, IM_COL32(60, 30, 35, 235), IM_COL32(90, 40, 45, 255), btnPrs, txtCol)) {
            s_isOpen = false;
        }
        curX += closeW + gap;

        if (Key(4, 1, 5, "<", curX, curY, arrowW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
            if (s_cursorPos > 0) {
                int p = s_cursorPos - 1;
                while (p > 0 && (static_cast<uint8_t>(s_targetBuf[p]) & 0xC0) == 0x80) {
                    p--;
                }
                s_cursorPos = p;
            }
        }
        curX += arrowW + gap;

        if (Key(4, 2, 6, ">", curX, curY, arrowW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
            int curLen = s_targetBuf ? static_cast<int>(strlen(s_targetBuf)) : 0;
            if (s_cursorPos < curLen) {
                int p = s_cursorPos + 1;
                while (p < curLen && (static_cast<uint8_t>(s_targetBuf[p]) & 0xC0) == 0x80) {
                    p++;
                }
                s_cursorPos = p;
            }
        }
        curX += arrowW + gap;

        if (Key(4, 3, 7, "SPACE", curX, curY, spaceW, btnH, btnBg, btnHov, btnPrs, txtCol)) {
            InsertTextAtCursor(" ");
        }
        curX += spaceW + gap;

        ImU32 jBg  = IM_COL32(0, 204, 166, (int)(220 * s_kbAnim));
        ImU32 jHov = IM_COL32(25, 242, 204, (int)(255 * s_kbAnim));
        if (Key(4, 4, 8, s_submitLabel.c_str(), curX, curY, jumpW, btnH, jBg, jHov, btnPrs, txtPrs)) {
            if (s_onSubmitCb) s_onSubmitCb();
            s_isOpen = false;
        }

        /* pad focus ring (pulsing cyan, like MpUi) */
        if (s_padMode && s_focusR >= 0 && s_focusR < 5 &&
            s_focusC >= 0 && s_focusC < kCols[s_focusR]) {
            const KbKey& kk = s_keys[s_focusR][s_focusC];
            float pulse = 0.85f + 0.15f * sinf((float)ImGui::GetTime() * 6.0f);
            ImU32 glow = IM_COL32(0, 195, 227, (int)(70 * s_kbAnim));
            ImU32 ring = IM_COL32(0, 195, 227, (int)(255 * pulse * s_kbAnim));
            draw->AddRect(ImVec2(kk.a.x - 5.0f * s, kk.a.y - 5.0f * s),
                          ImVec2(kk.b.x + 5.0f * s, kk.b.y + 5.0f * s),
                          glow, 9.0f * s, 0, 3.0f * s);
            draw->AddRect(ImVec2(kk.a.x - 2.0f * s, kk.a.y - 2.0f * s),
                          ImVec2(kk.b.x + 2.0f * s, kk.b.y + 2.0f * s),
                          ring, 6.0f * s, 0, 4.0f * s);
        }

        draw->PopClipRect();
    }
    ImGui::End();
}

} // namespace VirtualKeyboard
