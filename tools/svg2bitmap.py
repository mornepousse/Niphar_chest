#!/usr/bin/env python3
"""svg2bitmap — rasterise un SVG en bitmap 1 bit pour l'ecran OLED du coffre.

Bibliotheque standard seule (zlib, struct, subprocess...) : ni Pillow, ni
ImageMagick, ni cairosvg ne sont installes sur la machine de developpement, et
il n'y a pas de raison d'en faire une dependance du depot pour 64x64 pixels.

Le rasterisage SVG -> PNG est delegue a Inkscape (deja present, deja verifie
sur ce logo) via un appel externe ; le decodage PNG -> pixels est fait a la
main (chunks IHDR/IDAT, inflate zlib, filtres de reconstruction PNG standard)
pour ne rien devoir d'autre qu'a la bibliotheque standard.

Usage :
    python3 tools/svg2bitmap.py assets/nephar_logo_y.svg main/hmi/screen_logo.h \\
        --symbol screen_logo --size 64 --threshold 128

Regenere exactement main/hmi/screen_logo.h a partir de assets/nephar_logo_y.svg.
Voir le commentaire d'en-tete du fichier genere pour la commande figee.
"""

import argparse
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path


def read_png(path):
    """Decode un PNG en (largeur, hauteur, canaux, lignes de pixels bruts).

    Gere les cinq filtres PNG standard (None/Sub/Up/Average/Paeth) et les
    types de couleur courants en sortie Inkscape : gris (0), RVB (2),
    palette (3, non geree ici), gris+alpha (4), RVBA (6).
    """
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} n'est pas un PNG")

    pos = 8
    idat = b""
    width = height = bit_depth = color_type = None
    while pos < len(data):
        length, chunk_type = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type = struct.unpack(">IIBB", body[:10])
        elif chunk_type == b"IDAT":
            idat += body
        elif chunk_type == b"IEND":
            break
        pos += 12 + length  # longueur + type + donnees + CRC

    if width is None:
        raise ValueError(f"{path} : pas d'en-tete IHDR")
    if bit_depth != 8:
        raise ValueError(f"{path} : profondeur {bit_depth} bits non geree (attendu 8)")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"{path} : type de couleur {color_type} non gere")
    if color_type == 3:
        raise ValueError(f"{path} : PNG palette non gere — exporter sans palette")

    raw = zlib.decompress(idat)
    stride = width * channels

    rows = []
    prev = bytearray(stride)
    p = 0
    for _ in range(height):
        filt = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride

        if filt == 1:      # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filt == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:    # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:    # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif filt != 0:
            raise ValueError(f"filtre PNG inconnu : {filt}")

        rows.append(bytes(line))
        prev = line

    return width, height, channels, rows


def rasterize(svg_path, size):
    """Appelle Inkscape pour produire un PNG size x size, fond blanc opaque."""
    with tempfile.TemporaryDirectory() as tmp:
        png_path = Path(tmp) / "out.png"
        cmd = [
            "inkscape",
            "--export-type=png",
            f"--export-width={size}",
            f"--export-height={size}",
            "--export-background=white",
            "--export-background-opacity=1",
            f"--export-filename={png_path}",
            str(svg_path),
        ]
        subprocess.run(cmd, check=True, capture_output=True)
        return read_png(png_path)


def to_bitmap(width, height, channels, rows, threshold):
    """Seuille en 1 bit/pixel, MSB en premier, empaquete ligne par ligne.

    width doit etre un multiple de 8 : chaque ligne source tient alors dans un
    nombre entier d'octets, sans bit de bourrage a gerer au rendu (voir
    draw_logo() dans screen.c, qui indexe bit a bit sans tenir de reste).
    """
    if width % 8 != 0:
        raise ValueError(f"largeur {width} non multiple de 8")

    lit = 0
    out = bytearray(width // 8 * height)
    for y in range(height):
        row = rows[y]
        for x in range(width):
            px = row[x * channels:(x + 1) * channels]
            if channels == 4 and px[3] < 128:
                lum = 255  # transparent -> traite comme fond clair
            elif channels == 2 and px[1] < 128:
                lum = 255
            else:
                lum = px[0] if channels in (1, 2) else \
                    (px[0] * 299 + px[1] * 587 + px[2] * 114) // 1000
            if lum < threshold:
                lit += 1
                out[y * (width // 8) + x // 8] |= 0x80 >> (x % 8)
    return bytes(out), lit


def emit_header(out_path, symbol, width, height, data, lit, svg_rel, cmd_line):
    lines = []
    lines.append("#pragma once")
    lines.append("")
    lines.append("/*")
    lines.append(f" * {out_path.name} — logo Niphargus, {width}x{height}, 1 bit/pixel, genere.")
    lines.append(" *")
    lines.append(f" * Regenere depuis {svg_rel} par :")
    lines.append(" *")
    lines.append(f" *   {cmd_line}")
    lines.append(" *")
    lines.append(f" * {lit} pixels allumes sur {width * height} — silhouette verifiee a")
    lines.append(" * l'oeil sur l'apercu Inkscape avant integration (voir le rapport de")
    lines.append(" * tache de l'ecran d'accueil). Ne pas editer a la main : relancer le")
    lines.append(" * script si le SVG source change.")
    lines.append(" *")
    lines.append(" * Format : 1 = pixel allume, MSB en tete de chaque octet, ligne par")
    lines.append(f" * ligne ({width // 8} octets par ligne, {height} lignes, pas de bourrage —")
    lines.append(f" * {width} est un multiple de 8). Voir draw_logo() dans hmi/screen.c pour")
    lines.append(" * le blit vers le tampon de trame de l'ecran.")
    lines.append(" */")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define {symbol.upper()}_WIDTH  {width}u")
    lines.append(f"#define {symbol.upper()}_HEIGHT {height}u")
    lines.append("")
    lines.append(f"static const uint8_t {symbol}_bits[{len(data)}] = {{")
    per_line = 12
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    out_path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("svg", type=Path, help="SVG source (ex: assets/nephar_logo_y.svg)")
    ap.add_argument("out", type=Path, help="en-tete C a generer (ex: main/hmi/screen_logo.h)")
    ap.add_argument("--symbol", default="screen_logo", help="prefixe des symboles C generes")
    ap.add_argument("--size", type=int, default=64, help="cote en pixels (carre)")
    ap.add_argument("--threshold", type=int, default=128, help="seuil de luminance 0-255")
    args = ap.parse_args()

    if not args.svg.is_file():
        sys.exit(f"introuvable : {args.svg}")

    width, height, channels, rows = rasterize(args.svg, args.size)
    data, lit = to_bitmap(width, height, channels, rows, args.threshold)

    cmd_line = (
        f"python3 tools/svg2bitmap.py {args.svg.as_posix()} {args.out.as_posix()} "
        f"--symbol {args.symbol} --size {args.size} --threshold {args.threshold}"
    )
    emit_header(args.out, args.symbol, width, height, data, lit, args.svg.as_posix(), cmd_line)
    print(f"{args.out} : {width}x{height}, {lit} pixels allumes sur {width * height}, "
          f"{len(data)} octets")


if __name__ == "__main__":
    main()
