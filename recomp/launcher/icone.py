"""Icône du launcher (dessin original, vert néon sur noir) : écrit icone.ico à côté de ce script.

  python -I icone.py
"""
import os

from PIL import Image, ImageDraw, ImageFilter

ICI = os.path.dirname(os.path.abspath(__file__))
S = 1024   # dessin en grand, réduit ensuite


def dessiner():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((24, 24, S - 24, S - 24), radius=190, fill=(3, 8, 5, 255), outline=(40, 160, 80, 255), width=14)
    # flèche delta stylisée (vue de dessus), avec une encoche à l'arrière
    pointe = [(S * 0.50, S * 0.16), (S * 0.82, S * 0.80), (S * 0.50, S * 0.64), (S * 0.18, S * 0.80)]
    lueur = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(lueur).polygon(pointe, fill=(60, 255, 122, 255))
    lueur = lueur.filter(ImageFilter.GaussianBlur(46))
    im.alpha_composite(lueur)
    d = ImageDraw.Draw(im)
    d.polygon(pointe, fill=(60, 255, 122, 255))
    # arête centrale plus claire
    d.polygon([(S * 0.50, S * 0.16), (S * 0.56, S * 0.62), (S * 0.50, S * 0.64), (S * 0.44, S * 0.62)], fill=(200, 255, 220, 255))
    # ligne d'horizon
    d.rectangle((S * 0.22, S * 0.86, S * 0.78, S * 0.875), fill=(60, 255, 122, 200))
    return im


def main():
    im = dessiner()
    tailles = [16, 24, 32, 48, 64, 128, 256]
    im.resize((256, 256), Image.LANCZOS).save(os.path.join(ICI, "icone.ico"), sizes=[(t, t) for t in tailles])
    im.resize((256, 256), Image.LANCZOS).save(os.path.join(ICI, "icone.png"))
    print("icone.ico écrite")


if __name__ == "__main__":
    main()
