// Extraits vidéo des cinématiques (pour le launcher) : voir record.cpp.
#pragma once
#include <cstdint>

namespace hos {
namespace rec {

// L'image présentée numéro `frame` doit-elle être relue (enregistrement actif, une image sur deux) ?
bool wanted(uint64_t frame);
// Image présentée relue, réduite de moitié, en RGBA 8 bits (lignes de haut en bas).
void frame(const uint8_t* rgba, uint32_t w, uint32_t h, uint64_t frame);

}  // namespace rec
}  // namespace hos
