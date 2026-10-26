// nn::ngc::ProfanityFilter on the host. The SDK's version mounts the system's NG word list (system data
// 0x0100000000000823), which is not part of the game files, and aborts without it: nothing is filtered here.
#include <string.h>

#include "../hle.h"

HLE_FUNC("_ZN2nn3ngc15ProfanityFilterC1Ev", ngc_ctor) { (void)c; }  // (AEABI constructors return this: r0 kept)
HLE_FUNC("_ZN2nn3ngc15ProfanityFilterD1Ev", ngc_dtor) { (void)c; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvj", ngc_initialize) { RET(0); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", ngc_set_mask_mode) { (void)c; }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", ngc_content_version) { RET(1); }
// CheckProfanityWords(this, u32 *results, u32 pattern_mask, const char **words, size_t count)
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcj", ngc_check_words) {
    uint32_t results = ARG(1), count = STK(0);
    if (results) memset(G2H(results), 0, (size_t)count * 4);
    RET(0);
}
// MaskProfanityWordsInText(this, int *masked_count, char *text, u32 pattern_mask)
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ngc_mask_text) {
    if (ARG(1)) ST32(ARG(1), 0);
    RET(0);
}
