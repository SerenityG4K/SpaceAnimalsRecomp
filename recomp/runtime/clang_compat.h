// Code généré compilé par clang (build.py, compilateur_genere : -include clang_compat.h).
//
// [[clang::musttail]] exige que la fonction appelée ait la même signature que l'appelante, void f(CPUState*) ; le code
// généré appelle rt_tailcall(ctx, cible) pour un saut indirect (br) hors de la fonction. Ici la cible passe par
// ctx->pad (libre) et rt_tailjump(ctx), de même signature, la reprend (runtime.cpp). g++ 15+ accepte l'appel
// d'origine : ce fichier ne sert qu'à clang, et le texte généré reste le même pour les deux compilateurs.
#pragma once
#include "cpu.h"

void rt_tailjump(CPUState* ctx);
#define rt_tailcall(c, t) rt_tailjump(((c)->pad = (uint64_t)(t), (c)))
