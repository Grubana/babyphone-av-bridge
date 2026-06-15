#pragma once
#include <optional>
#include <vector>
#include "frame.h"

namespace babycam {

struct MediaUnit {
    enum Kind { Video, Audio } kind;
    bool keyframe = false;          // video only
    std::vector<uint8_t> data;      // video: Annex-B from first start code; audio: G.711 payload
};

std::optional<MediaUnit> extractMedia(const Frame& fr);

} // namespace babycam
