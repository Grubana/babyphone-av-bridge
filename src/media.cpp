#include "media.h"

namespace babycam {

static const uint8_t SC[4] = {0x00, 0x00, 0x00, 0x01};

std::optional<MediaUnit> extractMedia(const Frame& fr) {
    if (fr.type != 8 || fr.body.size() < 2) return std::nullopt;
    int op = fr.op();
    if (op == 0x0000 || op == 0x0100) {
        for (size_t i = 0; i + 4 <= fr.body.size(); ++i) {
            if (fr.body[i] == SC[0] && fr.body[i+1] == SC[1] &&
                fr.body[i+2] == SC[2] && fr.body[i+3] == SC[3]) {
                MediaUnit m; m.kind = MediaUnit::Video; m.keyframe = (op == 0x0000);
                m.data.assign(fr.body.begin() + i, fr.body.end());
                return m;
            }
        }
        return std::nullopt;
    }
    if (op == 0x0201) {
        if (fr.body.size() < 20) return std::nullopt;
        MediaUnit m; m.kind = MediaUnit::Audio;
        m.data.assign(fr.body.begin() + 20, fr.body.end());
        return m;
    }
    return std::nullopt;
}

} // namespace babycam
