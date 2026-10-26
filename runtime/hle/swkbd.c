// Software keyboard. On the Switch it is a library applet; here the physical keyboard types (runtime/textinput.c):
//  - the inline keyboard (the only one the game was seen to use: the hunter's name at character creation...):
//    while it is shown, what is typed goes to the game's changed-string callback (the game shows the text in its own
//    entry box), Enter to its decided-enter callback, Escape to decided-cancel;
//  - ShowKeyboard (the full-screen keyboard applet; not seen used) answers "Hunter".
//    User words (the earlier entries as suggestions): taken and released at once (Calc calls the released callback).
// Tests: MHGU_NAME=<text> answers every entry with that text at once, as typed and confirmed (MHGU_INLINE_CANCEL=1:
// cancels instead); MHGU_CONTROL's "type" / "textkey" commands type into an entry.
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../textinput.h"

// nn::swkbd::ShowKeyboard(String *out, const ShowKeyboardArg &) -> Result. String = {void *ptr; size_t size}.
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint32_t ptr = LD32(ARG(0)), size = LD32(ARG(0) + 4);
    const char *name = getenv("MHGU_NAME");
    if (!name || !*name) name = "Hunter";
    if (ptr && size) {
        memset(G2H(ptr), 0, size);
        snprintf((char *)G2H(ptr), size, "%s", name);
    }
    LOG("[swkbd] keyboard applet answered: \"%s\"\n", name);
    RET(0);
}
HLE_FUNC("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb", swkbd_work_size) { RET(0xD000); }
HLE_FUNC("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv", swkbd_string_size) { RET(0x7D4); }

// ---- inline keyboard: callbacks the game set, called from Calc on its own thread
static uint32_t cb_init_done, cb_cancel, cb_enter, cb_changed, cb_moved, cb_released, init_pending;
// Set/UnsetUserWordInfo: the keyboard takes the word list (the player's earlier entries, as suggestions) and hands the
// buffer back with the released-user-word-info callback, from Calc. The game waits for it before the keyboard appears
// (the item set names of a character with earlier entries: without it, a 15 s dimmed screen and no keyboard).
static int words_pending;
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboardC1Ev", ik_ctor) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboardD1Ev", ik_dtor) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard25GetRequiredWorkBufferSizeEv", ik_work_size) { RET(0x40000); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", ik_initialize) { init_pending = 1; RET(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", ik_finalize) { textinput_end(); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", ik_cb_init) { cb_init_done = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", ik_cb_cancel) { cb_cancel = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard27SetDecidedEnterCallbackUtf8EPFvPKNS0_19DecidedEnterArgUtf8EE", ik_cb_enter) { cb_enter = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard28SetChangedStringCallbackUtf8EPFvPKNS0_20ChangedStringArgUtf8EE", ik_cb_changed) { cb_changed = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard26SetMovedCursorCallbackUtf8EPFvPKNS0_18MovedCursorArgUtf8EE", ik_cb_moved) { cb_moved = ARG(1); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", ik_cb_finished) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard31SetReleasedUserWordInfoCallbackEPFvvE", ik_cb_released) { cb_released = ARG(1); }

// The callbacks' argument (Changed/DecidedEnter/MovedCursor ArgUtf8, as the game reads them): the text at +0 (up to
// 0x7d4 bytes), its length in characters at +0x7d4, then the cursor position (+0x7d8; ChangedString also has the
// dictionary range there: given the cursor too).
#define ARG_TEXT_MAX 0x7d0
static uint32_t cb_arg(const char *text, int chars) {
    static uint32_t arg;
    if (!arg) arg = hle_alloc(0x800);
    memset(G2H(arg), 0, 0x800);
    snprintf((char *)G2H(arg), ARG_TEXT_MAX, "%s", text);
    ST32(arg + 0x7d4, (uint32_t)chars);
    for (uint32_t o = 0x7d8; o <= 0x7e0; o += 4) ST32(o + arg, (uint32_t)chars);
    return arg;
}

static void callback(Cpu *c, uint32_t fn, uint32_t arg) {
    if (!fn) return;
    c->r[0] = arg;
    guest_call(c, fn);
}

// Calc returns the keyboard's state (as the applet reports it): 0 not initialized, 1 ready and hidden, 3 shown. The
// game's keyboard driver waits for 1 after a text entry (its "closed" step).
static int ik_state;
static char ik_initial[ARG_TEXT_MAX + 4];
static int ik_auto;  // MHGU_NAME / MHGU_INLINE_CANCEL: answered on the second Calc after Appear
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard4CalcEv", ik_calc) {
    uint32_t saved[4] = {c->r[0], c->r[1], c->r[2], c->r[3]};
    if (init_pending && cb_init_done) { init_pending = 0; guest_call(c, cb_init_done); }
    if (init_pending == 0 && ik_state == 0) ik_state = 1;
    if (words_pending && cb_released) { words_pending = 0; guest_call(c, cb_released); }
    if (ik_state == 3 && ik_auto && ++ik_auto > 2) {
        const char *name = getenv("MHGU_NAME");
        if (getenv("MHGU_INLINE_CANCEL")) callback(c, cb_cancel, 0);
        else callback(c, cb_enter, cb_arg(name ? name : "Hunter", (int)strlen(name ? name : "Hunter")));
        ik_auto = 0;
        ik_state = 1;
    } else if (ik_state == 3 && !ik_auto) {
        static char text[ARG_TEXT_MAX + 4];
        int chars, ev = textinput_poll(text, sizeof text, &chars);
        if (ev == TI_CHANGED) {
            uint32_t arg = cb_arg(text, chars);
            callback(c, cb_changed, arg);
            callback(c, cb_moved, arg);
        } else if (ev == TI_ENTER) {
            LOG("[swkbd] entered \"%s\"\n", text);
            textinput_end();
            callback(c, cb_enter, cb_arg(text, chars));
            ik_state = 1;
        } else if (ev == TI_CANCEL) {
            LOG("[swkbd] text entry cancelled\n");
            textinput_end();
            callback(c, cb_cancel, 0);
            ik_state = 1;
        }
    }
    memcpy(c->r, saved, sizeof saved);
    RET(ik_state);
}
// AppearArg: u32 keyboard mode, char16 ok text[9], 2 x char16 optional symbol keys, 2 x bool, u32 invalid
// characters, s32 max length (+0x20), s32 min length.
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", ik_appear) {
    uint32_t a = ARG(1);
    int max = (int)LD32(a + 0x20);
    if (max <= 0 || max > 500) max = 0;
    char hex[3 * 0x30 + 1];
    for (int i = 0; i < 0x30; i++) snprintf(hex + 3 * i, 4, " %02x", LD8(a + (uint32_t)i));
    LOG("[swkbd] inline keyboard shown (frame %llu), AppearArg%s\n", (unsigned long long)g_present_count, hex);
    ik_state = 3;
    ik_auto = getenv("MHGU_NAME") || getenv("MHGU_INLINE_CANCEL") ? 1 : 0;
    if (!ik_auto) textinput_begin(ik_initial, max);
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard9DisappearEv", ik_disappear) {
    if (ik_state == 3) LOG("[swkbd] inline keyboard closed by the game\n");
    textinput_end();
    if (ik_state == 3) ik_state = 1;
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard11SetUtf8ModeEb", ik_utf8) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi", ik_cursor) { (void)c; }
// InputText: the text the entry starts with (the name being changed), UTF-16 inline at +0 (500 characters at most)
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE", ik_input_text) {
    uint32_t a = ARG(1);
    size_t n = 0;
    for (uint32_t i = 0; i < 500; i++) {
        uint32_t u = LD16(a + 2 * i);
        if (!u) break;
        if (u >= 0xD800 && u < 0xDC00) {  // a surrogate pair
            uint32_t lo = LD16(a + 2 * i + 2);
            if (lo < 0xDC00 || lo >= 0xE000) break;
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        } else if (u >= 0xDC00 && u < 0xE000) {
            break;
        }
        uint8_t b[4];
        size_t k = u < 0x80 ? (b[0] = (uint8_t)u, 1)
                 : u < 0x800 ? (b[0] = (uint8_t)(0xC0 | u >> 6), b[1] = (uint8_t)(0x80 | (u & 0x3F)), 2)
                 : u < 0x10000 ? (b[0] = (uint8_t)(0xE0 | u >> 12), b[1] = (uint8_t)(0x80 | (u >> 6 & 0x3F)),
                                  b[2] = (uint8_t)(0x80 | (u & 0x3F)), 3)
                 : (b[0] = (uint8_t)(0xF0 | u >> 18), b[1] = (uint8_t)(0x80 | (u >> 12 & 0x3F)),
                    b[2] = (uint8_t)(0x80 | (u >> 6 & 0x3F)), b[3] = (uint8_t)(0x80 | (u & 0x3F)), 4);
        if (n + k > ARG_TEXT_MAX) break;
        memcpy(ik_initial + n, b, k);
        n += k;
    }
    ik_initial[n] = 0;
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard14SetKeytopAlphaEf", ik_alpha) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard14SetKeytopScaleEf", ik_scale) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard18SetKeytopTranslateEff", ik_translate) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard15SetCustomizeDicEPKvjRKNS0_16CustomizeDicInfoE", ik_dic) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard17UnsetCustomizeDicEv", ik_undic) { (void)c; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard15SetUserWordInfoERKNS0_12UserWordInfoE", ik_words) {
    (void)c;
    words_pending = 1;
    LOG("[swkbd] user word info set (released on the next Calc)\n");
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard17UnsetUserWordInfoEv", ik_unwords) { (void)c; words_pending = 1; }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard25GetImageMemoryRequirementEPjS2_", ik_image_req) {
    if (ARG(1)) ST32(ARG(1), 0x10000);
    if (ARG(2)) ST32(ARG(2), 0x1000);
}
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard8GetImageEPvj", ik_image) { RET(0); }
HLE_FUNC("_ZN2nn5swkbd14InlineKeyboard26IsUsedTouchPointByKeyboardEii", ik_touch) { RET(0); }
HLE_FUNC("_ZNK2nn5swkbd14InlineKeyboard13GetWindowSizeEPiS2_", ik_window_size) {
    if (ARG(1)) ST32(ARG(1), 0);
    if (ARG(2)) ST32(ARG(2), 0);
}
HLE_FUNC("_ZN2nn5swkbd12UserWordInfo25GetRequiredWorkBufferSizeEi", swkbd_words_size) { RET(0x100 + ARG(0) * 0x80); }
