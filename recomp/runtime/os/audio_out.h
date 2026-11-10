// Sortie audio de l'hôte (audio_out.cpp) : périphérique Windows par défaut, 48 kHz, stéréo, flottant.
#pragma once
#include <cstdint>

namespace hos {
namespace audio_out {

// Rappel de mixage : écrit `frames` trames stéréo flottantes (48 kHz, entrelacées) dans dst.
// Appelé par le fil de rendu audio, au rythme de l'horloge de la carte son.
using MixFn = void (*)(float* dst, uint32_t frames);

// Ouvre le périphérique dans un fil dédié (nouvel essai s'il manque ou disparaît).
// RECOMP_NO_AUDIO=1 : aucune sortie (les flux restent consommés par l'horloge virtuelle du HLE).
void start(MixFn mix);
// Vrai quand le périphérique est ouvert : c'est alors lui qui consomme les flux du jeu.
bool active();

}  // namespace audio_out
}  // namespace hos
