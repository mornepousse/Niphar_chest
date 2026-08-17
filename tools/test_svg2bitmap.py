#!/usr/bin/env python3
"""Tests du decodeur PNG de svg2bitmap.py.

Pourquoi ces tests existent : le decodage PNG a la main (chunks, inflate, les
cinq filtres de reconstruction) est exactement le « parsing d'en-tetes » que la
norme TDD du projet cible. Un bug dans le predicteur Paeth ne casse pas le
build, ne leve aucune exception, et ne se voit qu'a l'oeil sur un logo deja
committe — donc jamais.

La propriete centrale n'est pas un fichier temoin, c'est une invariance : un
meme bitmap encode avec chacun des cinq filtres PNG doit se decoder aux memes
pixels. Un filtre casse se trahit tout seul, sans qu'on ait a savoir a l'avance
quels octets attendre.

Lance par scripts/fast.sh. Ne demande ni Inkscape ni reseau : les PNG sont
fabriques ici, en memoire, par la bibliotheque standard.
"""

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from svg2bitmap import read_png, to_bitmap  # noqa: E402


# --------------------------------------------------------------------------- #
# Encodeur PNG minimal, cote test uniquement — le miroir exact des filtres que
# read_png() doit savoir defaire.
# --------------------------------------------------------------------------- #

def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)


def _filter_line(filt, line, prev, bpp):
    """Applique le filtre PNG `filt` vers l'avant (encodage)."""
    out = bytearray(len(line))
    for i in range(len(line)):
        a = line[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if filt == 0:
            pred = 0
        elif filt == 1:
            pred = a
        elif filt == 2:
            pred = b
        elif filt == 3:
            pred = (a + b) >> 1
        elif filt == 4:
            pred = _paeth(a, b, c)
        else:
            raise ValueError(filt)
        out[i] = (line[i] - pred) & 0xFF
    return bytes(out)


def _chunk(kind, body):
    return (struct.pack(">I", len(body)) + kind + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))


def _make_png(rows, channels, filt, color_type=None):
    """Fabrique un PNG 8 bits a partir de lignes brutes, filtre uniforme `filt`.

    `filt` peut etre un entier (meme filtre partout) ou une liste, un par ligne :
    un PNG reel melange les filtres ligne a ligne, et c'est ce cas melange qui
    fait porter au decodeur la bonne ligne `prev`.
    """
    if color_type is None:
        color_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    width = len(rows[0]) // channels
    height = len(rows)
    filts = [filt] * height if isinstance(filt, int) else list(filt)

    raw = b""
    prev = bytes(len(rows[0]))
    for y, line in enumerate(rows):
        raw += bytes([filts[y]]) + _filter_line(filts[y], line, prev, channels)
        prev = line

    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(raw)) + _chunk(b"IEND", b""))


def _decode(blob):
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / "t.png"
        p.write_bytes(blob)
        return read_png(p)


# Motif de test : 8x4 en RVB. Choisi pour que chaque filtre ait du travail —
# les voisins de gauche, du dessus et de la diagonale diffferent tous, et les
# valeurs traversent 0 et 255 pour exercer le repliement modulo 256.
PATTERN_RGB = [
    bytes([(x * 31 + y * 7) & 0xFF for x in range(8 * 3)])
    for y in range(4)
]


class TestFilterInvariance(unittest.TestCase):
    """Les cinq filtres doivent rendre exactement les memes pixels."""

    def test_five_filters_agree(self):
        reference = None
        for filt in range(5):
            w, h, ch, rows = _decode(_make_png(PATTERN_RGB, 3, filt))
            self.assertEqual((w, h, ch), (8, 4, 3), f"geometrie, filtre {filt}")
            if reference is None:
                reference = rows
            else:
                self.assertEqual(rows, reference,
                                 f"le filtre {filt} ne rend pas les memes pixels "
                                 f"que le filtre 0")
        # Et le filtre 0 rend bien l'original, sinon les cinq pourraient
        # s'accorder sur une meme erreur.
        self.assertEqual(reference, [bytes(r) for r in PATTERN_RGB])

    def test_mixed_filters_per_line(self):
        """Un filtre different par ligne — le cas des PNG reels."""
        _, _, _, rows = _decode(_make_png(PATTERN_RGB, 3, [0, 4, 2, 3]))
        self.assertEqual(rows, [bytes(r) for r in PATTERN_RGB])

    def test_each_filter_alone_on_greyscale(self):
        """Un seul canal : `bpp` valant 1, les bornes de debut de ligne changent."""
        grey = [bytes([(x * 53 + y * 101) & 0xFF for x in range(8)]) for y in range(4)]
        for filt in range(5):
            _, _, ch, rows = _decode(_make_png(grey, 1, filt))
            self.assertEqual(ch, 1)
            self.assertEqual(rows, [bytes(r) for r in grey], f"filtre {filt}, gris")


class TestHeaderHandling(unittest.TestCase):
    def test_rejects_non_png(self):
        with self.assertRaises(ValueError):
            _decode(b"pas du tout un png, meme pas la signature")

    def test_rejects_unknown_filter(self):
        blob = bytearray(_make_png(PATTERN_RGB, 3, 0))
        # Reconstruit un IDAT dont la premiere ligne annonce le filtre 7.
        raw = bytearray(b"\x00" + bytes(PATTERN_RGB[0]))
        for line in PATTERN_RGB[1:]:
            raw += b"\x00" + bytes(line)
        raw[0] = 7
        ihdr = struct.pack(">IIBBBBB", 8, 4, 8, 2, 0, 0, 0)
        blob = (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
                + _chunk(b"IDAT", zlib.compress(bytes(raw))) + _chunk(b"IEND", b""))
        with self.assertRaises(ValueError):
            _decode(blob)

    def test_rejects_palette(self):
        blob = _make_png([bytes(range(8))], 1, 0, color_type=3)
        with self.assertRaises(ValueError):
            _decode(blob)

    def test_rejects_16_bit_depth(self):
        ihdr = struct.pack(">IIBBBBB", 8, 1, 16, 0, 0, 0, 0)
        blob = (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
                + _chunk(b"IDAT", zlib.compress(b"\x00" + bytes(16)))
                + _chunk(b"IEND", b""))
        with self.assertRaises(ValueError):
            _decode(blob)

    def test_split_idat_chunks_are_concatenated(self):
        """Un PNG reel coupe souvent IDAT en plusieurs chunks."""
        raw = b""
        prev = bytes(24)
        for line in PATTERN_RGB:
            raw += b"\x02" + _filter_line(2, line, prev, 3)
            prev = line
        comp = zlib.compress(raw)
        cut = len(comp) // 2
        ihdr = struct.pack(">IIBBBBB", 8, 4, 8, 2, 0, 0, 0)
        blob = (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr)
                + _chunk(b"IDAT", comp[:cut]) + _chunk(b"IDAT", comp[cut:])
                + _chunk(b"IEND", b""))
        _, _, _, rows = _decode(blob)
        self.assertEqual(rows, [bytes(r) for r in PATTERN_RGB])


class TestToBitmap(unittest.TestCase):
    """Le seuillage et l'empaquetage : c'est ce qui finit dans la flash."""

    def test_msb_first_packing(self):
        # Une ligne de 8 pixels gris : seul le premier est noir.
        rows = [bytes([0, 255, 255, 255, 255, 255, 255, 255])]
        data, lit = to_bitmap(8, 1, 1, rows, 128)
        self.assertEqual(lit, 1)
        self.assertEqual(data, b"\x80", "le premier pixel doit etre le MSB")

    def test_last_pixel_is_lsb(self):
        rows = [bytes([255] * 7 + [0])]
        data, lit = to_bitmap(8, 1, 1, rows, 128)
        self.assertEqual((data, lit), (b"\x01", 1))

    def test_threshold_is_strict(self):
        """lum < seuil allume. A lum == seuil, le pixel reste eteint."""
        rows = [bytes([127, 128, 129, 255, 255, 255, 255, 255])]
        data, lit = to_bitmap(8, 1, 1, rows, 128)
        self.assertEqual(lit, 1, "seul 127 est sous le seuil de 128")
        self.assertEqual(data, b"\x80")

    def test_rgb_luminance_weights(self):
        """Le vert pese plus que le rouge, qui pese plus que le bleu.

        Aux poids 299/587/114 sur 1000, un canal sature seul donne : rouge 76,
        vert 149, bleu 29. Le seuil est place a 100, entre le vert et le rouge —
        c'est le seul reglage qui distingue les trois. Un seuil a 500 les
        allumerait tous les trois et le test ne mordrait sur rien.
        """
        rows = [bytes([255, 0, 0,   0, 255, 0,   0, 0, 255]
                      + [255, 255, 255] * 5)]
        data, lit = to_bitmap(8, 1, 3, rows, 100)
        self.assertEqual(lit, 2, "rouge (76) et bleu (29) sous 100, vert (149) au-dessus")
        self.assertEqual(data[0] & 0x80, 0x80, "rouge pur : lum 76 < 100")
        self.assertEqual(data[0] & 0x40, 0x00, "vert pur : lum 149 >= 100")
        self.assertEqual(data[0] & 0x20, 0x20, "bleu pur : lum 29 < 100")

    def _measured_luminance(self, r, g, b):
        """Fait dire sa luminance a to_bitmap, en balayant le seuil.

        Le pixel s'allume si `lum < seuil` : le plus petit seuil qui l'allume
        vaut donc lum + 1. On ne recopie pas la formule ici — la recopier
        rendrait le test tautologique, vert quels que soient les coefficients
        reellement appliques par le code.
        """
        for seuil in range(0, 257):
            _, lit = to_bitmap(8, 1, 3, [bytes([r, g, b] + [255, 255, 255] * 7)], seuil)
            if lit >= 1:
                return seuil - 1
        self.fail(f"({r},{g},{b}) ne s'allume a aucun seuil")

    def test_green_outweighs_red_outweighs_blue(self):
        """Les poids sont ordonnes, pas seulement presents.

        Une permutation des trois coefficients garderait le test precedent vert
        si elle laissait le vert le plus lourd. Celui-ci fixe les trois rangs, et
        les trois valeurs, mesurees sur le code.
        """
        rouge = self._measured_luminance(255, 0, 0)
        vert = self._measured_luminance(0, 255, 0)
        bleu = self._measured_luminance(0, 0, 255)
        self.assertEqual((rouge, vert, bleu), (76, 149, 29),
                         "poids attendus 299/587/114 sur 1000")
        self.assertLess(bleu, rouge, "le bleu doit peser moins que le rouge")
        self.assertLess(rouge, vert, "le rouge doit peser moins que le vert")

    def test_transparent_counts_as_light(self):
        """Un pixel transparent est du fond, pas de l'encre — meme noir."""
        # RVBA : noir opaque, puis noir transparent.
        rows = [bytes([0, 0, 0, 255,  0, 0, 0, 0] + [255, 255, 255, 255] * 6)]
        data, lit = to_bitmap(8, 1, 4, rows, 128)
        self.assertEqual(lit, 1, "le noir transparent ne doit pas s'allumer")
        self.assertEqual(data, b"\x80")

    def test_rejects_width_not_multiple_of_8(self):
        with self.assertRaises(ValueError):
            to_bitmap(7, 1, 1, [bytes(7)], 128)

    def test_row_stride_across_multiple_bytes(self):
        """16 px de large : le pixel 8 doit etre le MSB du deuxieme octet."""
        rows = [bytes([255] * 8 + [0] + [255] * 7)]
        data, lit = to_bitmap(16, 1, 1, rows, 128)
        self.assertEqual((len(data), lit), (2, 1))
        self.assertEqual(data, b"\x00\x80")


if __name__ == "__main__":
    unittest.main(verbosity=2)
