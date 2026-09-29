#!/usr/bin/env python3
"""Tests de la logique pure de tools/niphar-oath.

Lancés par scripts/fast.sh, comme tools/test_svg2bitmap.py — donc SANS pyusb :
tout ce qui est testé ici doit rester importable sur une machine qui n'a pas la
bibliothèque. C'est la raison pour laquelle `import usb` est différé dans le
client, au fond de la classe de transport, et jamais en tête de fichier.

Ce qui est éprouvé ici est exactement ce qu'aucune carte ne rattraperait :
  - le modulo que la carte NE FAIT PAS (§2 du brief) ;
  - le compteur de temps gros-boutien (§1) ;
  - l'absorption des trames WTX pendant l'attente de l'appui (§3).
Une erreur sur l'un des trois donne un client qui « marche » et ment.
"""

import builtins
import contextlib
import importlib.util
import io
import os
import sys
import unittest

_ICI = os.path.dirname(os.path.abspath(__file__))
_CLIENT = os.path.join(_ICI, "niphar-oath")

_spec = importlib.util.spec_from_loader(
    "niphar_oath",
    importlib.machinery.SourceFileLoader("niphar_oath", _CLIENT),
)
oath = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(oath)


# --------------------------------------------------------------------------
# TLV
# --------------------------------------------------------------------------
class TestTlv(unittest.TestCase):
    def test_encodage_tag_longueur_valeur(self):
        self.assertEqual(oath.tlv(0x71, b"abc"), b"\x71\x03abc")

    def test_encodage_valeur_vide(self):
        # 0x7C « appui requis » est un TLV SANS valeur — pas un TLV absent.
        self.assertEqual(oath.tlv(0x7C, b""), b"\x7c\x00")

    def test_refuse_valeur_de_plus_de_255_octets(self):
        with self.assertRaises(ValueError):
            oath.tlv(0x73, b"x" * 256)

    def test_analyse_rend_les_tags_dans_l_ordre(self):
        # Deux 0x71 de suite : c'est la forme du RENAME, et un analyseur qui
        # déduplique par tag perdrait le second.
        brut = oath.tlv(0x71, b"vieux") + oath.tlv(0x71, b"neuf")
        self.assertEqual(
            oath.tlv_parse(brut), [(0x71, b"vieux"), (0x71, b"neuf")]
        )

    def test_analyse_refuse_une_longueur_qui_deborde(self):
        with self.assertRaises(oath.OathError):
            oath.tlv_parse(b"\x71\x08ab")

    def test_analyse_refuse_un_tlv_tronque(self):
        with self.assertRaises(oath.OathError):
            oath.tlv_parse(b"\x71")

    def test_first_rend_none_quand_le_tag_manque(self):
        self.assertIsNone(oath.tlv_first(oath.tlv_parse(b"\x79\x01\x05"), 0x71))


# --------------------------------------------------------------------------
# APDU
# --------------------------------------------------------------------------
class TestApdu(unittest.TestCase):
    def test_cas_1_entete_seul(self):
        # SEND REMAINING : quatre octets, pas de Lc, pas de Le. apdu_parse du
        # firmware traite len==4 comme le cas 1 ; y ajouter un Le le ferait
        # basculer en cas 2S.
        self.assertEqual(oath.apdu(0x00, 0xA5, 0x00, 0x00), b"\x00\xa5\x00\x00")

    def test_cas_3_court_avec_donnees(self):
        self.assertEqual(
            oath.apdu(0x00, 0x01, 0x00, 0x00, b"\xaa\xbb"),
            b"\x00\x01\x00\x00\x02\xaa\xbb",
        )

    def test_refuse_plus_de_255_octets_de_donnees(self):
        # Le firmware ne lit que des APDU courtes pour ces commandes ; tronquer
        # en silence provisionnerait un secret amputé.
        with self.assertRaises(ValueError):
            oath.apdu(0x00, 0x01, 0x00, 0x00, b"x" * 256)

    def test_select_porte_l_aid_ykoath(self):
        self.assertEqual(
            oath.apdu_select(),
            b"\x00\xa4\x04\x00\x07\xa0\x00\x00\x05\x27\x21\x01",
        )

    def test_calculate_demande_la_troncature(self):
        # P2=01 : c'est ce qui fait répondre un 0x76 plutôt qu'un 0x75.
        trame = oath.apdu_calculate("A", b"\x00" * 8)
        self.assertEqual(trame[:4], b"\x00\xa2\x00\x01")
        self.assertEqual(
            oath.tlv_parse(trame[5:]),
            [(0x71, b"A"), (0x74, b"\x00" * 8)],
        )

    def test_put_encode_type_chiffres_puis_secret(self):
        trame = oath.apdu_put("A", b"\xde\xad", 6)
        self.assertEqual(trame[:4], b"\x00\x01\x00\x00")
        self.assertEqual(
            oath.tlv_parse(trame[5:]),
            [(0x71, b"A"), (0x73, b"\x21\x06\xde\xad")],
        )

    def test_put_refuse_un_nombre_de_chiffres_hors_6_ou_8(self):
        with self.assertRaises(ValueError):
            oath.apdu_put("A", b"\x01", 7)


# --------------------------------------------------------------------------
# Compteur de temps
# --------------------------------------------------------------------------
class TestChallenge(unittest.TestCase):
    def test_huit_octets_gros_boutien(self):
        # 59 s / 30 = pas 1 — vecteur de la RFC 6238.
        self.assertEqual(
            oath.totp_challenge(59, 30), b"\x00\x00\x00\x00\x00\x00\x00\x01"
        )

    def test_pas_1234567890(self):
        self.assertEqual(
            oath.totp_challenge(1111111109, 30),
            b"\x00\x00\x00\x00\x02\x35\x23\xec",
        )

    def test_toujours_huit_octets(self):
        for t in (0, 1, 2**31, 2_000_000_000):
            self.assertEqual(len(oath.totp_challenge(t, 30)), 8)

    def test_periode_differente_donne_un_pas_different(self):
        # Une période lue de travers rend un code plausible et faux : les deux
        # valeurs doivent DIVERGER, pas seulement exister.
        self.assertNotEqual(
            oath.totp_challenge(1_000_000, 30), oath.totp_challenge(1_000_000, 60)
        )

    def test_gros_boutien_et_non_petit(self):
        # Un struct.pack("<q") passerait tous les tests de longueur ci-dessus.
        self.assertEqual(oath.totp_challenge(30 * 0x0102030405060708, 30)[0], 0x01)


class TestPeriode(unittest.TestCase):
    def test_nom_sans_prefixe_vaut_trente(self):
        self.assertEqual(oath.parse_period("GitHub:mae"), (30, "GitHub:mae"))

    def test_prefixe_numerique_donne_la_periode(self):
        self.assertEqual(oath.parse_period("60/GitHub:mae"), (60, "GitHub:mae"))

    def test_un_slash_sans_chiffres_n_est_pas_une_periode(self):
        self.assertEqual(oath.parse_period("a/b"), (30, "a/b"))

    def test_periode_nulle_refusee(self):
        with self.assertRaises(ValueError):
            oath.parse_period("0/x")


# --------------------------------------------------------------------------
# Le modulo, que la carte ne fait pas
# --------------------------------------------------------------------------
class TestFormatCode(unittest.TestCase):
    def test_modulo_applique_sur_six_chiffres(self):
        # 0x00000539 = 1337 ; six chiffres → « 001337 ».
        self.assertEqual(oath.format_code(6, b"\x00\x00\x05\x39"), "001337")

    def test_zero_padding_conserve(self):
        self.assertEqual(oath.format_code(6, b"\x00\x00\x00\x01"), "000001")
        self.assertEqual(oath.format_code(8, b"\x00\x00\x00\x01"), "00000001")

    def test_bit_de_poids_fort_masque(self):
        # 0xFFFFFFFF & 0x7FFFFFFF = 2147483647 ; % 10**6 = 483647.
        self.assertEqual(oath.format_code(6, b"\xff\xff\xff\xff"), "483647")

    def test_le_modulo_enroule_vraiment(self):
        # 0x000F4240 = 1 000 000 pile : six chiffres doivent donner « 000000 »,
        # huit « 01000000 ». Un client qui se contenterait de zfill(6) sur la
        # valeur brute rendrait « 1000000 » — sept chiffres, et faux.
        self.assertEqual(oath.format_code(6, b"\x00\x0f\x42\x40"), "000000")
        self.assertEqual(oath.format_code(8, b"\x00\x0f\x42\x40"), "01000000")

    def test_ne_rend_jamais_les_octets_bruts(self):
        brut = b"\x12\x34\x56\x78"
        self.assertEqual(len(oath.format_code(6, brut)), 6)
        self.assertNotIn(brut.hex(), oath.format_code(8, brut))

    def test_deux_valeurs_brutes_distinctes_donnent_deux_codes_distincts(self):
        self.assertNotEqual(
            oath.format_code(6, b"\x00\x00\x00\x01"),
            oath.format_code(6, b"\x00\x00\x00\x02"),
        )

    def test_refuse_autre_chose_que_quatre_octets(self):
        with self.assertRaises(oath.OathError):
            oath.format_code(6, b"\x00\x00\x00")

    def test_reponse_tronquee_complete(self):
        # 0x76 = [chiffres][4 octets bruts] : c'est la carte qui donne les
        # chiffres, jamais un défaut posé côté hôte.
        self.assertEqual(
            oath.parse_truncated(oath.tlv(0x76, b"\x08\x00\x00\x05\x39")), "00001337"
        )

    def test_reponse_tronquee_refuse_un_0x75(self):
        # 0x75 est la réponse NON tronquée (HMAC complet) : la prendre pour un
        # 0x76 rendrait un code calculé sur les mauvais octets.
        with self.assertRaises(oath.OathError):
            oath.parse_truncated(oath.tlv(0x75, b"\x06" + b"\xaa" * 20))


# --------------------------------------------------------------------------
# Secret base32
# --------------------------------------------------------------------------
class TestSecret(unittest.TestCase):
    def test_decodage_simple(self):
        self.assertEqual(oath.decode_secret("MZXW6==="), b"foo")

    def test_padding_ajoute(self):
        self.assertEqual(oath.decode_secret("MZXW6"), b"foo")

    def test_minuscules_et_espaces_acceptes(self):
        self.assertEqual(oath.decode_secret("mzxw 6"), b"foo")

    def test_refuse_un_alphabet_invalide(self):
        with self.assertRaises(ValueError):
            oath.decode_secret("MZXW61")  # 1 n'est pas dans l'alphabet base32

    def test_refuse_un_secret_vide(self):
        with self.assertRaises(ValueError):
            oath.decode_secret("")

    def test_refuse_plus_de_64_octets(self):
        # SEC_SECRET_MAX = 64 : la carte répondrait 6A80, autant le dire ici.
        with self.assertRaises(ValueError):
            oath.decode_secret("A" * 128)


# --------------------------------------------------------------------------
# Trames CCID — le point où un client naïf échoue
# --------------------------------------------------------------------------
def _wtx(seq=0):
    return bytes([0x80, 0, 0, 0, 0, 0, seq, 0x80, 0x02, 0x00])


def _datablock(payload, seq=0, status=0x00, error=0x00):
    return (
        bytes([0x80])
        + len(payload).to_bytes(4, "little")
        + bytes([0, seq, status, error, 0x00])
        + payload
    )


class TestWtx(unittest.TestCase):
    def test_une_trame_wtx_est_reconnue(self):
        self.assertTrue(oath.is_wtx(_wtx()))

    def test_une_reponse_finale_n_est_pas_un_wtx(self):
        self.assertFalse(oath.is_wtx(_datablock(b"\x90\x00")))

    def test_un_echec_n_est_pas_un_wtx(self):
        # bmCommandStatus = 01b (échec) — surtout pas à confondre avec 10b
        # (extension de temps), sans quoi le client attendrait une réponse qui
        # ne viendra jamais.
        self.assertFalse(oath.is_wtx(_datablock(b"", status=0x40)))

    def test_une_trame_slot_status_n_est_pas_un_wtx(self):
        self.assertFalse(oath.is_wtx(bytes([0x81, 0, 0, 0, 0, 0, 0, 0x80, 0, 0])))


class _Lecteur:
    """Fausse lecture bulk IN : rend les morceaux prévus, un par appel."""

    def __init__(self, morceaux):
        self.morceaux = list(morceaux)
        self.appels = 0

    def __call__(self):
        self.appels += 1
        if not self.morceaux:
            raise AssertionError("lecture au-delà de ce qui était prévu")
        return self.morceaux.pop(0)


class TestLectureCcid(unittest.TestCase):
    def test_message_recolle_sur_plusieurs_paquets(self):
        msg = _datablock(b"\xaa" * 300)
        lecteur = _Lecteur([msg[:64], msg[64:200], msg[200:]])
        self.assertEqual(oath.read_ccid_message(lecteur), msg)

    def test_zlp_ignoree(self):
        msg = _datablock(b"\x90\x00")
        lecteur = _Lecteur([b"", msg])
        self.assertEqual(oath.read_ccid_message(lecteur), msg)

    def test_octets_en_trop_ne_sont_pas_rendus(self):
        msg = _datablock(b"\x90\x00")
        lecteur = _Lecteur([msg + b"\xff\xff"])
        self.assertEqual(oath.read_ccid_message(lecteur), msg)

    def test_attente_absorbe_dix_wtx_avant_la_reponse(self):
        # LE test du brief §3 : dix extensions de temps (15 s d'attente d'appui)
        # puis la vraie réponse. Un client qui prend le premier WTX pour une
        # réponse rendrait ici une trame vide sans jamais s'en apercevoir.
        final = _datablock(oath.tlv(0x76, b"\x06\x00\x00\x05\x39") + b"\x90\x00")
        lecteur = _Lecteur([_wtx()] * 10 + [final])
        horloge = iter([0.0] + [i * 1.5 for i in range(1, 40)])
        recu = oath.await_final_frame(lecteur, 30.0, now=lambda: next(horloge))
        self.assertEqual(recu, final)
        self.assertEqual(lecteur.appels, 11)

    def test_attente_expire_si_les_wtx_ne_cessent_jamais(self):
        lecteur = _Lecteur([_wtx()] * 100)
        horloge = iter([i * 1.5 for i in range(200)])
        with self.assertRaises(oath.CcidError):
            oath.await_final_frame(lecteur, 20.0, now=lambda: next(horloge))

    def test_echec_ccid_leve_une_erreur(self):
        lecteur = _Lecteur([_datablock(b"", status=0x40, error=0xFE)])
        with self.assertRaises(oath.CcidError):
            oath.await_final_frame(lecteur, 5.0, now=lambda: 0.0)

    def test_charge_utile_extraite_de_la_trame(self):
        self.assertEqual(oath.ccid_payload(_datablock(b"\x90\x00")), b"\x90\x00")

    def test_entete_xfrblock(self):
        # bMessageType 0x6F, dwLength en petit-boutien, bSlot puis bSeq.
        self.assertEqual(
            oath.ccid_xfr(b"\x00\xa1\x00\x00", seq=7),
            bytes([0x6F, 4, 0, 0, 0, 0, 7, 0, 0, 0]) + b"\x00\xa1\x00\x00",
        )


# --------------------------------------------------------------------------
# Couche APDU : mots d'état et chaînage
# --------------------------------------------------------------------------
class TestMotDEtat(unittest.TestCase):
    def test_decoupe_donnees_et_mot_d_etat(self):
        self.assertEqual(oath.split_sw(b"\xaa\xbb\x90\x00"), (b"\xaa\xbb", 0x9000))

    def test_refuse_une_reponse_trop_courte(self):
        with self.assertRaises(oath.OathError):
            oath.split_sw(b"\x90")

    def test_61xx_demande_la_suite(self):
        self.assertTrue(oath.sw_has_more(0x6100))
        self.assertTrue(oath.sw_has_more(0x61FF))

    def test_9000_ne_demande_rien(self):
        self.assertFalse(oath.sw_has_more(0x9000))
        self.assertFalse(oath.sw_has_more(0x6A82))

    def test_les_mots_d_etat_du_firmware_sont_nommes(self):
        # Ces quatre-là sont ceux que Mae verra le plus : un « 6985 » nu ne dit
        # pas qu'elle n'a pas appuyé.
        for sw in (0x6985, 0x6A82, 0x6A84, 0x6A81):
            self.assertIn(sw, oath.SW_NOMS)
            self.assertTrue(oath.SW_NOMS[sw])

    def test_message_d_erreur_cite_le_mot_d_etat_inconnu(self):
        self.assertIn("6f00", oath.sw_message(0x6F00).lower())


# --------------------------------------------------------------------------
# Réponses de l'applet
# --------------------------------------------------------------------------
class TestReponses(unittest.TestCase):
    def test_select_rend_version_et_sel(self):
        body = oath.tlv(0x79, b"\x05\x07\x01") + oath.tlv(0x71, b"\x01" * 8)
        version, sel = oath.parse_select(body)
        self.assertEqual(version, "5.7.1")
        self.assertEqual(sel, b"\x01" * 8)

    def test_select_sans_version_est_refuse(self):
        with self.assertRaises(oath.OathError):
            oath.parse_select(oath.tlv(0x71, b"\x01" * 8))

    def test_liste_rend_algo_et_nom(self):
        # Le nom sort en OCTETS BRUTS : c'est sur eux que la clé compare, et
        # tout décodage au milieu de la comparaison est un décodage à perte.
        body = oath.tlv(0x72, b"\x21GitHub:mae") + oath.tlv(0x72, b"\x21b")
        self.assertEqual(
            oath.parse_list(body), [(0x21, b"GitHub:mae"), (0x21, b"b")]
        )

    def test_liste_ne_decode_pas_une_etiquette_non_utf8(self):
        """LE test de m7 : une étiquette qui n'est pas de l'UTF-8 valide.

        Avec l'ancien décodage `backslashreplace`, l'octet 0xFF ressortait en
        « \\xff » — six caractères qui ne se comparent plus ni à ce que Mae
        tape ni à ce que la clé stocke. `cmd_add()` en concluait « ce compte
        n'existe pas », employait PLAIN_TIMEOUT_S (6 s) sur une commande que la
        clé tient QUINZE secondes, et abandonnait avec la commande en vol —
        ce que la docstring d'await_final_frame() interdit explicitement.
        """
        brut = b"OVH:\xffperso"
        body = oath.tlv(0x72, bytes([0x21]) + brut)
        self.assertEqual(oath.parse_list(body), [(0x21, brut)])
        # Le round-trip est exact : c'est la propriété dont dépend la décision
        # « ce compte existe-t-il ? ».
        self.assertEqual(oath.check_name(brut), brut)
        # Et l'affichage, lui, reste lisible — mais À PERTE, donc jamais
        # employé pour décider.
        self.assertIn("OVH", oath.nom_affichable(brut))

    def test_liste_vide(self):
        self.assertEqual(oath.parse_list(b""), [])

    def test_liste_refuse_une_entree_sans_octet_d_algo(self):
        with self.assertRaises(oath.OathError):
            oath.parse_list(oath.tlv(0x72, b""))

    def test_liste_ignore_les_tags_etrangers(self):
        body = oath.tlv(0x79, b"\x05") + oath.tlv(0x72, b"\x21a")
        self.assertEqual(oath.parse_list(body), [(0x21, b"a")])

    def test_algo_lisible(self):
        self.assertEqual(oath.algo_nom(0x21), "TOTP/SHA1")
        self.assertIn("0x11", oath.algo_nom(0x11))


# --------------------------------------------------------------------------
# D'où vient le secret — C1 : jamais de la ligne de commande
# --------------------------------------------------------------------------
class _Flux:
    """Une entrée standard factice, dont on choisit si elle est un terminal."""

    def __init__(self, texte="", tty=False):
        self._texte = texte
        self._tty = tty

    def isatty(self):
        return self._tty

    def readline(self):
        ligne, _, reste = self._texte.partition("\n")
        if not self._texte:
            return ""
        self._texte = reste
        return ligne + "\n"

    def read(self):
        t, self._texte = self._texte, ""
        return t


class TestSecretHorsArgv(unittest.TestCase):
    def test_un_tuyau_donne_le_secret_sur_l_entree_standard(self):
        flux = _Flux("JBSWY3DPEHPK3PXP\n", tty=False)
        self.assertEqual(
            oath.secret_depuis_entree(None, stdin=flux), "JBSWY3DPEHPK3PXP"
        )

    def test_un_terminal_passe_par_une_invite_sans_echo(self):
        # getpass n'écho rien et ne passe par aucun historique : c'est LA
        # raison d'être de la branche interactive. On vérifie qu'elle est bien
        # empruntée, et que l'entrée standard n'est PAS lue à la place.
        appels = []

        def faux_getpass(invite):
            appels.append(invite)
            return "  JBSWY3DPEHPK3PXP  "

        flux = _Flux("CECI-NE-DOIT-PAS-ETRE-LU\n", tty=True)
        got = oath.secret_depuis_entree(None, stdin=flux, getpass_fn=faux_getpass)
        self.assertEqual(got, "JBSWY3DPEHPK3PXP")
        self.assertEqual(len(appels), 1)

    def test_le_tiret_vaut_absence(self):
        flux = _Flux("SECRET\n", tty=False)
        self.assertEqual(oath.secret_depuis_entree("-", stdin=flux), "SECRET")

    def test_un_secret_explicite_reste_accepte(self):
        # L'argument reste utilisable pour les scripts — son aide dit ce qu'il
        # en coûte. Le vérifier interdit de « corriger » C1 en cassant l'usage
        # scripté sans le dire.
        flux = _Flux("AUTRE\n", tty=False)
        self.assertEqual(oath.secret_depuis_entree("EXPLICITE", stdin=flux),
                         "EXPLICITE")

    def test_entree_standard_vide_est_une_erreur_nommee(self):
        with self.assertRaises(oath.OathError):
            oath.secret_depuis_entree(None, stdin=_Flux("", tty=False))

    def test_une_ligne_blanche_ne_passe_pas_pour_un_secret(self):
        with self.assertRaises(oath.OathError):
            oath.secret_depuis_entree(None, stdin=_Flux("   \n", tty=False))


# --------------------------------------------------------------------------
# Le lot — C1 : douze comptes, deux appareils, zéro saisie manuelle
# --------------------------------------------------------------------------
class TestLot(unittest.TestCase):
    def test_deux_champs_separes_par_un_blanc(self):
        r = oath.parse_batch("GitHub:mae JBSWY3DPEHPK3PXP\n")
        self.assertEqual(len(r), 1)
        numero, nom, secret, err = r[0]
        self.assertEqual((numero, nom, err), (1, "GitHub:mae", None))
        self.assertEqual(secret, oath.decode_secret("JBSWY3DPEHPK3PXP"))

    def test_la_tabulation_separe_aussi(self):
        r = oath.parse_batch("GitHub:mae\tJBSWY3DPEHPK3PXP\n")
        self.assertEqual(r[0][1], "GitHub:mae")
        self.assertIsNone(r[0][3])

    def test_lignes_vides_et_commentaires_ignores(self):
        texte = "# export Proton\n\nA:x JBSWY3DPEHPK3PXP\n   \n"
        r = oath.parse_batch(texte)
        self.assertEqual([l[1] for l in r], ["A:x"])

    def test_une_ligne_mal_formee_n_arrete_pas_les_suivantes(self):
        """LE point du lot : rendre compte de TOUT, ne s'arrêter sur rien.

        Une migration à moitié faite dont on ignore la moitié est pire qu'un
        échec net. Les trois lignes suivantes se suivent : bonne, cassée,
        bonne — la troisième doit être analysée quand même.
        """
        texte = ("A:x JBSWY3DPEHPK3PXP\n"
                 "B:y\n"                       # un seul champ
                 "C:z JBSWY3DPEHPK3PXP\n")
        r = oath.parse_batch(texte)
        self.assertEqual(len(r), 3)
        self.assertIsNone(r[0][3])
        self.assertIsNotNone(r[1][3])
        self.assertIsNone(r[2][3], "la ligne d'APRÈS l'erreur est traitée")

    def test_trois_champs_sont_refuses_plutot_que_devines(self):
        # « OVH:mae dupont SECRET » : couper au premier blanc mettrait
        # « dupont » en tête du secret, et « dupontJBSW… » reste du base32
        # valide — le compte serait provisionné avec un secret FAUX, en
        # silence. Un refus nommé vaut mieux qu'une devinette muette.
        r = oath.parse_batch("OVH:mae dupont JBSWY3DPEHPK3PXP\n")
        self.assertEqual(len(r), 1)
        self.assertIsNone(r[0][2])
        self.assertIn("deux attendus", r[0][3])

    def test_un_secret_base32_invalide_est_nomme_avant_toute_ecriture(self):
        r = oath.parse_batch("A:x PAS!DU!BASE32\n")
        self.assertIsNone(r[0][2])
        self.assertIsNotNone(r[0][3])

    def test_un_nom_trop_long_est_rejete_comme_la_cle_le_ferait(self):
        long_nom = "x" * oath.SEC_LABEL_LEN
        r = oath.parse_batch(f"{long_nom} JBSWY3DPEHPK3PXP\n")
        self.assertIsNone(r[0][2])
        self.assertIsNotNone(r[0][3])

    def test_un_doublon_dans_le_lot_est_refuse(self):
        # Provisionner puis écraser demanderait un appui au milieu d'une
        # migration automatique, sans que personne sache lequel des deux
        # secrets a gagné.
        texte = "A:x JBSWY3DPEHPK3PXP\nA:x MZXW6YTBOI======\n"
        r = oath.parse_batch(texte)
        self.assertIsNone(r[0][3])
        self.assertIsNotNone(r[1][3])

    def test_les_numeros_de_ligne_designent_le_fichier_d_origine(self):
        # Compter les lignes EXPLOITABLES ferait désigner la mauvaise ligne
        # dans le rapport, donc envoyer Mae corriger un compte qui va bien.
        texte = "# entête\n\nA:x PAS!DU!BASE32\n"
        r = oath.parse_batch(texte)
        self.assertEqual(r[0][0], 3)


# --------------------------------------------------------------------------
# RESET — I5 : le rempart côté hôte, avant tout envoi
# --------------------------------------------------------------------------
class TestResetHote(unittest.TestCase):
    def test_le_texte_nomme_chaque_compte_et_leur_nombre(self):
        # « 12 comptes » ne permet pas de reconnaître qu'on s'est trompé de
        # clé ; l'écran de la clé ne peut afficher que le nombre, donc c'est à
        # l'hôte — qui a la place — de dire le reste.
        t = oath.texte_reset([b"OVH:perso", b"OVH:pro", b"GitHub:mae"])
        for nom in ("OVH:perso", "OVH:pro", "GitHub:mae"):
            self.assertIn(nom, t)
        self.assertIn("3", t)

    def test_un_magasin_vide_le_dit(self):
        t = oath.texte_reset([])
        self.assertIn("aucun compte", t.lower())

    def test_le_mot_exact_est_exige(self):
        self.assertTrue(oath.reponse_confirme_le_reset("EFFACER"))
        self.assertTrue(oath.reponse_confirme_le_reset("  EFFACER \n"))

    def test_tout_le_reste_renonce(self):
        # « oui » et un Entrée réflexe sont exactement les deux saisies qu'un
        # geste automatique produit : elles ne doivent rien autoriser.
        for saisie in ("", "  ", "oui", "o", "y", "effacer", "Effacer",
                       "EFFACER TOUT", "EFFACE", None):
            self.assertFalse(oath.reponse_confirme_le_reset(saisie),
                             f"« {saisie} » ne doit pas autoriser le RESET")


# --------------------------------------------------------------------------
# RENAME et RESET — les trames, I5
# --------------------------------------------------------------------------
class TestApduDestructrices(unittest.TestCase):
    def test_reset_porte_les_deux_octets_de_verrouillage(self):
        # P1/P2 = DE:AD SONT le verrou : c'est leur unique raison d'être, et
        # sans eux la clé répond 6A80 (OATH_RESET_P1/P2, oath_proto.c).
        self.assertEqual(oath.apdu_reset(), b"\x00\x04\xde\xad")

    def test_reset_est_une_apdu_de_cas_1(self):
        # Quatre octets, pas de Lc ni de Le : apdu_parse() du firmware traite
        # len==4 comme le cas 1. Un octet de plus en ferait une autre commande.
        self.assertEqual(len(oath.apdu_reset()), 4)

    def test_rename_porte_deux_0x71_dans_l_ordre(self):
        # oath_do_rename() lit le PREMIER 0x71 comme la CIBLE et cherche le
        # second dans ce qui SUIT sa valeur : les inverser renommerait un
        # compte inconnu, ou le compte en lui-même sans erreur visible.
        trame = oath.apdu_rename("vieux", "neuf")
        self.assertEqual(trame[:4], b"\x00\x05\x00\x00")
        tlvs = oath.tlv_parse(trame[5:])
        self.assertEqual(tlvs, [(0x71, b"vieux"), (0x71, b"neuf")])

    def test_rename_refuse_un_nom_identique(self):
        with self.assertRaises(ValueError):
            oath.apdu_rename("pareil", "pareil")

    def test_rename_borne_les_deux_noms(self):
        with self.assertRaises(ValueError):
            oath.apdu_rename("a", "x" * oath.SEC_LABEL_LEN)
        with self.assertRaises(ValueError):
            oath.apdu_rename("", "b")


# ===========================================================================
# LE CÂBLAGE — un faux transport, pour éprouver la couche cmd_* sans matériel
# ===========================================================================
# Ce qui manquait : `secret_depuis_entree()` et `parse_batch()` sont éprouvées
# une par une, mais RIEN ne vérifiait que `cmd_add()` les appelle. Une re-revue
# par mutation a trouvé neuf survivantes, toutes dans ce câblage — dont trois
# qui remettent les douze graines TOTP en clair dans ~/.zsh_history.
#
# Le faux transport se substitue au SEUL point qui touche libusb (`Cle._xfr`) :
# tout ce qui est au-dessus — `exchange()`, le chaînage 61xx, `expect_ok()`,
# `parse_list()` et les `cmd_*` — reste le vrai code, et les APDU envoyées sont
# enregistrées telles quelles. Une commande qui n'aurait pas dû partir se voit
# donc, au lieu de se déduire d'un message affiché.


class FauxTransport(oath.Cle):
    """Transport CCID factice : enregistre les APDU, répond de façon réglable.

    `comptes` : le magasin initial, en octets bruts (ce que rend un LIST).
    `echecs`  : {nom -> mot d'état} pour faire échouer un PUT, un DELETE ou un
                RENAME (indexé sur le nom PUT, ou sur l'ANCIEN nom pour RENAME)
                proprement.
    `explosions` : les noms dont l'envoi lève, comme un câble arraché.
    `code_digits`/`code_raw` : la réponse à un CALCULATE — [chiffres][4 octets
                bruts], exactement la forme d'un 0x76. `None` par défaut :
                CALCULATE répond alors un 9000 nu (pas de TLV), comme les
                autres INS non simulés.
    """

    def __init__(self, comptes=(), echecs=None, explosions=(),
                 code_digits=None, code_raw=None):
        super().__init__(verbose=False)
        self.comptes = [oath.check_name(c) for c in comptes]
        self.envoyees = []
        self.echecs = dict(echecs or {})
        self.explosions = {oath.check_name(n) for n in explosions}
        self.code_digits = code_digits
        self.code_raw = code_raw

    def _xfr(self, apdu_bytes, timeout_s):
        apdu_bytes = bytes(apdu_bytes)
        self.envoyees.append(apdu_bytes)
        ins = apdu_bytes[1]
        if ins == oath.INS_LIST:
            corps = b"".join(
                oath.tlv(oath.TAG_NAME_LIST,
                         bytes([oath.ALGO_TOTP_SHA1]) + nom)
                for nom in self.comptes)
            return corps + b"\x90\x00"
        if ins == oath.INS_PUT:
            nom = oath.tlv_first(oath.tlv_parse(apdu_bytes[5:]), oath.TAG_NAME)
            if nom in self.explosions:
                raise oath.CcidError("transport factice : rien reçu de la clé")
            sw = self.echecs.get(nom, 0x9000)
            if sw == 0x9000 and nom not in self.comptes:
                self.comptes.append(nom)
            return bytes([sw >> 8, sw & 0xFF])
        if ins == oath.INS_DELETE:
            nom = oath.tlv_first(oath.tlv_parse(apdu_bytes[5:]), oath.TAG_NAME)
            sw = self.echecs.get(nom, 0x9000)
            if sw == 0x9000 and nom in self.comptes:
                self.comptes.remove(nom)
            return bytes([sw >> 8, sw & 0xFF])
        if ins == oath.INS_RENAME:
            tlvs = oath.tlv_parse(apdu_bytes[5:])
            noms = [v for t, v in tlvs if t == oath.TAG_NAME]
            ancien = noms[0] if noms else None
            sw = self.echecs.get(ancien, 0x9000)
            if sw == 0x9000 and len(noms) == 2 and ancien in self.comptes:
                self.comptes[self.comptes.index(ancien)] = noms[1]
            return bytes([sw >> 8, sw & 0xFF])
        if ins == oath.INS_CALCULATE:
            nom = oath.tlv_first(oath.tlv_parse(apdu_bytes[5:]), oath.TAG_NAME)
            sw = self.echecs.get(nom, 0x9000)
            if sw != 0x9000:
                return bytes([sw >> 8, sw & 0xFF])
            if self.code_digits is not None:
                corps = oath.tlv(oath.TAG_TRUNCATED,
                                  bytes([self.code_digits]) + self.code_raw)
                return corps + b"\x90\x00"
            return b"\x90\x00"
        return b"\x90\x00"

    # -- lectures pour les assertions --------------------------------------
    def ins_envoyees(self):
        return [trame[1] for trame in self.envoyees]

    def puts(self):
        """[(nom, secret, chiffres)] pour chaque PUT réellement parti."""
        out = []
        for trame in self.envoyees:
            if trame[1] != oath.INS_PUT:
                continue
            tlvs = oath.tlv_parse(trame[5:])
            nom = oath.tlv_first(tlvs, oath.TAG_NAME)
            cle = oath.tlv_first(tlvs, oath.TAG_KEY)
            out.append((nom, cle[2:], cle[1]))
        return out


@contextlib.contextmanager
def _entree(flux):
    """Substitue l'entrée standard du processus, et la rend ensuite.

    `cmd_add`, `cmd_add_batch` et `cmd_reset` lisent `sys.stdin` directement —
    c'est précisément ce câblage-là qu'on éprouve, donc on ne peut pas se
    contenter des paramètres injectables de `secret_depuis_entree()`.
    """
    ancien = sys.stdin
    sys.stdin = flux
    try:
        yield
    finally:
        sys.stdin = ancien


@contextlib.contextmanager
def _saisie(reponse):
    """Substitue `input()`. `reponse` peut lever (EOFError) ou rendre du texte."""
    ancien = builtins.input
    builtins.input = (reponse if callable(reponse) else (lambda _="": reponse))
    try:
        yield
    finally:
        builtins.input = ancien


@contextlib.contextmanager
def _muet():
    """Absorbe les deux sorties. Rend le tampon de la sortie standard."""
    tampon = io.StringIO()
    with contextlib.redirect_stdout(tampon), \
            contextlib.redirect_stderr(io.StringIO()):
        yield tampon


def _args(*argv):
    """Les arguments TELS QUE LA LIGNE DE COMMANDE les produit.

    Passer par `build_parser()` plutôt que par un objet fabriqué à la main :
    un défaut d'argparse qui change (le positionnel `secret` redevenu
    obligatoire, par exemple) doit se voir ici aussi.
    """
    return oath.build_parser().parse_args(list(argv))


# --------------------------------------------------------------------------
# C1 — le secret ne passe PAS par argv, et c'est cmd_add qui doit le garantir
# --------------------------------------------------------------------------
class TestCablageAdd(unittest.TestCase):
    def test_sans_argument_le_secret_vient_de_l_entree_standard(self):
        """La survivante la plus grave : `decode_secret(args.secret)`.

        `secret_depuis_entree()` est éprouvée à part, mais rien ne vérifiait
        que `cmd_add()` l'appelle. La relire directement remet les douze
        graines en clair dans ~/.zsh_history et dans /proc/<pid>/cmdline.
        """
        cle = FauxTransport()
        with _entree(_Flux("JBSWY3DPEHPK3PXP\n", tty=False)), _muet():
            rc = oath.cmd_add(cle, _args("add", "GitHub:mae"))
        self.assertEqual(rc, 0)
        self.assertEqual(
            cle.puts(),
            [(b"GitHub:mae", oath.decode_secret("JBSWY3DPEHPK3PXP"), 6)])

    def test_un_terminal_passe_par_l_invite_sans_echo(self):
        # L'autre branche du même appel : sur un vrai terminal, le secret se
        # tape sans écho. Un `cmd_add` qui lirait argv ne consulterait ni
        # l'une ni l'autre.
        cle = FauxTransport()
        invites = []

        def faux_getpass(invite):
            invites.append(invite)
            return "JBSWY3DPEHPK3PXP"

        ancien = oath.getpass.getpass
        oath.getpass.getpass = faux_getpass
        try:
            with _entree(_Flux("NE-DOIT-PAS-ETRE-LU\n", tty=True)), _muet():
                rc = oath.cmd_add(cle, _args("add", "GitHub:mae"))
        finally:
            oath.getpass.getpass = ancien
        self.assertEqual(rc, 0)
        self.assertEqual(len(invites), 1)
        self.assertEqual(cle.puts()[0][1],
                         oath.decode_secret("JBSWY3DPEHPK3PXP"))

    def test_le_tiret_ne_part_pas_en_secret(self):
        # « - » est la forme documentée de « lis ailleurs ». Le décoder tel
        # quel provisionnerait un compte avec un secret vide — ou lèverait,
        # selon la mutation, mais jamais le bon secret.
        cle = FauxTransport()
        with _entree(_Flux("JBSWY3DPEHPK3PXP\n", tty=False)), _muet():
            rc = oath.cmd_add(cle, _args("add", "A:x", "-"))
        self.assertEqual(rc, 0)
        self.assertEqual(cle.puts()[0][1],
                         oath.decode_secret("JBSWY3DPEHPK3PXP"))

    def test_un_secret_explicite_reste_honore(self):
        # Le témoin dans l'autre sens : « corriger » C1 en cassant l'usage
        # scripté ne doit pas passer pour une amélioration.
        cle = FauxTransport()
        with _entree(_Flux("AUTRE\n", tty=False)), _muet():
            oath.cmd_add(cle, _args("add", "A:x", "JBSWY3DPEHPK3PXP"))
        self.assertEqual(cle.puts()[0][1],
                         oath.decode_secret("JBSWY3DPEHPK3PXP"))

    def test_le_secret_positionnel_reste_facultatif(self):
        # Le rendre obligatoire (nargs= retiré) forcerait Mae à le taper dans
        # le shell : la fuite revient par la porte de l'analyseur d'arguments.
        args = _args("add", "GitHub:mae")
        self.assertIsNone(args.secret)
        self.assertEqual(args.digits, 6)


# --------------------------------------------------------------------------
# C1 — le lot ne lit QUE l'entrée standard, jamais un fichier
# --------------------------------------------------------------------------
class TestCablageAddBatch(unittest.TestCase):
    def test_aucun_fichier_en_argument(self):
        """La contrainte est que les secrets ne touchent jamais le disque.

        Un positionnel ou une option de chemin suffirait à faire écrire les
        douze graines dans un fichier — qui survit à l'opération, et que rien
        n'efface.
        """
        for argv in (["add-batch", "/tmp/secrets.txt"],
                     ["add-batch", "--fichier", "/tmp/secrets.txt"],
                     ["add-batch", "--file", "/tmp/secrets.txt"],
                     ["add-batch", "-f", "/tmp/secrets.txt"]):
            with self.subTest(argv=argv), _muet():
                with self.assertRaises(SystemExit):
                    oath.build_parser().parse_args(argv)

    def test_un_envoi_qui_leve_n_arrete_pas_les_suivants(self):
        """Une migration à moitié faite dont on ignore la moitié est pire.

        La ligne B explose au transport. A a déjà été provisionné, C doit
        l'être aussi — et le compte rendu doit dire lequel des trois manque.
        """
        cle = FauxTransport(explosions=[b"B:y"])
        texte = ("A:x JBSWY3DPEHPK3PXP\n"
                 "B:y MZXW6YTBOI======\n"
                 "C:z JBSWY3DPEHPK3PXP\n")
        with _entree(_Flux(texte, tty=False)), _muet():
            rc = oath.cmd_add_batch(cle, _args("add-batch"))
        self.assertEqual(rc, 1, "un rejet doit se voir dans le code de sortie")
        self.assertEqual([nom for nom, _, _ in cle.puts()],
                         [b"A:x", b"B:y", b"C:z"])

    def test_un_mot_d_etat_en_echec_n_arrete_pas_les_suivants(self):
        # L'autre forme d'échec : la clé répond, mais refuse (magasin plein).
        # Elle passe par une autre branche que l'exception ci-dessus.
        cle = FauxTransport(echecs={b"B:y": 0x6A84})
        texte = ("A:x JBSWY3DPEHPK3PXP\n"
                 "B:y MZXW6YTBOI======\n"
                 "C:z JBSWY3DPEHPK3PXP\n")
        with _entree(_Flux(texte, tty=False)), _muet() as sortie:
            rc = oath.cmd_add_batch(cle, _args("add-batch"))
        self.assertEqual(rc, 1)
        self.assertEqual([nom for nom, _, _ in cle.puts()],
                         [b"A:x", b"B:y", b"C:z"])
        self.assertIn("2 ajouté(s)", sortie.getvalue())

    def test_un_compte_deja_present_n_est_pas_ecrase(self):
        """Écraser exige un appui physique, que personne ne donne douze fois.

        Sans ce filtre, une migration relancée par précaution demanderait un
        appui au milieu du lot — ou, pire, passerait si l'appui tombe par
        hasard, et remplacerait un secret vivant par celui de l'export.
        """
        cle = FauxTransport(comptes=[b"A:x"])
        texte = "A:x JBSWY3DPEHPK3PXP\nB:y MZXW6YTBOI======\n"
        with _entree(_Flux(texte, tty=False)), _muet() as sortie:
            rc = oath.cmd_add_batch(cle, _args("add-batch"))
        self.assertEqual(rc, 0, "un compte déjà là n'est pas un rejet")
        self.assertEqual([nom for nom, _, _ in cle.puts()], [b"B:y"])
        self.assertIn("déjà", sortie.getvalue())

    def test_le_lot_provisionne_bien_ce_qu_on_lui_donne(self):
        # Témoin positif : sans lui, les trois tests ci-dessus resteraient
        # verts sur un `cmd_add_batch` qui n'envoie jamais rien.
        cle = FauxTransport()
        texte = "A:x JBSWY3DPEHPK3PXP\nB:y MZXW6YTBOI======\n"
        with _entree(_Flux(texte, tty=False)), _muet():
            rc = oath.cmd_add_batch(cle, _args("add-batch", "--digits", "8"))
        self.assertEqual(rc, 0)
        self.assertEqual(
            cle.puts(),
            [(b"A:x", oath.decode_secret("JBSWY3DPEHPK3PXP"), 8),
             (b"B:y", oath.decode_secret("MZXW6YTBOI======"), 8)])


# --------------------------------------------------------------------------
# CODE — l'INS, P2, le nom et le défi ; et le modulo, qui vient du compte
# --------------------------------------------------------------------------
class TestCablageCode(unittest.TestCase):
    def test_calculate_porte_le_bon_ins_p2_nom_et_le_defi_gros_boutien(self):
        # Une date fixe, choisie loin de tout octet nul accidentel, pour que
        # le test ne dépende pas de l'horloge au moment où il tourne : le
        # client lit `time.time()` lui-même, jamais injectée par argv.
        cle = FauxTransport(
            comptes=[b"A:premier", b"GitHub:mae", b"Z:dernier"],
            code_digits=6, code_raw=b"\x00\x00\x05\x39",
        )
        ancien = oath.time.time
        oath.time.time = lambda: 30_000_000.0  # pas RFC 6238 = 1 000 000
        try:
            with _muet():
                rc = oath.cmd_code(cle, _args("code", "GitHub:mae"))
        finally:
            oath.time.time = ancien
        self.assertEqual(rc, 0)
        trame = cle.envoyees[-1]
        self.assertEqual(trame[1], oath.INS_CALCULATE)   # A2
        self.assertEqual(trame[2:4], b"\x00\x01")          # P1=00 P2=01 : troncature
        tlvs = oath.tlv_parse(trame[5:])
        self.assertEqual(oath.tlv_first(tlvs, oath.TAG_NAME), b"GitHub:mae")
        defi = oath.tlv_first(tlvs, oath.TAG_CHALLENGE)
        self.assertEqual(len(defi), 8)
        self.assertEqual(defi, oath.totp_challenge(30_000_000, 30))
        # Gros-boutien noir sur blanc : le pas (1 000 000 = 0x0F4240) tient
        # sur les QUATRE DERNIERS octets. Un petit-boutien le mettrait en
        # TÊTE de la trame, pas en queue — un HMAC parfaitement formé et faux.
        self.assertEqual(defi, b"\x00\x00\x00\x00\x00\x0f\x42\x40")

    def test_le_nombre_de_chiffres_vient_du_compte_pas_d_une_constante(self):
        """La carte annonce 8 chiffres dans son 0x76 : le client doit les
        suivre, jamais appliquer un module à six chiffres codé en dur.

        0x000F4240 = 1 000 000 pile : à six chiffres « 000000 », à huit
        « 01000000 ». Un `cmd_code` qui forcerait 6 chiffres afficherait le
        premier alors que la clé a demandé le second — un code plausible et
        faux, la panne la plus coûteuse à diagnostiquer (docstring de
        `totp_challenge`).
        """
        cle = FauxTransport(comptes=[b"A:x"], code_digits=8,
                             code_raw=b"\x00\x0f\x42\x40")
        with _muet() as sortie:
            rc = oath.cmd_code(cle, _args("code", "A:x"))
        self.assertEqual(rc, 0)
        self.assertIn("01000000", sortie.getvalue())

    def test_un_mot_d_etat_en_echec_n_affiche_aucun_code(self):
        # Témoin du sens inverse : un CALCULATE qui échoue ne doit pas
        # afficher un code — sans quoi le message de succès ci-dessus ne
        # prouverait rien de la branche « sw == 0x9000 ».
        cle = FauxTransport(comptes=[b"A:x"], echecs={b"A:x": 0x6985})
        with _muet() as sortie:
            rc = oath.cmd_code(cle, _args("code", "A:x"))
        self.assertEqual(rc, 1)
        self.assertNotIn("valide encore", sortie.getvalue())


# --------------------------------------------------------------------------
# DELETE — l'INS, le nom, et le BON compte (ni le premier, ni le dernier)
# --------------------------------------------------------------------------
class TestCablageDelete(unittest.TestCase):
    def test_delete_porte_le_bon_ins_et_vise_le_compte_nomme(self):
        # Le compte visé n'est NI le premier NI le dernier du magasin : un
        # DELETE qui enverrait toujours le premier (ou le dernier) compte de
        # la liste passerait un test qui ne viserait qu'une extrémité.
        cle = FauxTransport(comptes=[b"A:premier", b"B:milieu", b"C:dernier"])
        with _muet() as sortie:
            rc = oath.cmd_delete(cle, _args("delete", "B:milieu"))
        self.assertEqual(rc, 0)
        trame = cle.envoyees[-1]
        self.assertEqual(trame[1], oath.INS_DELETE)  # 02
        self.assertEqual(
            oath.tlv_parse(trame[5:]), [(oath.TAG_NAME, b"B:milieu")])
        self.assertIn("B:milieu", sortie.getvalue())
        # Témoin positif direct : le compte a VRAIMENT disparu du magasin —
        # sans ce contrôle, un DELETE qui n'enverrait qu'une APDU muette (sans
        # effet réel derrière) passerait le test ci-dessus quand même.
        self.assertEqual(cle.comptes, [b"A:premier", b"C:dernier"])

    def test_un_mot_d_etat_en_echec_ne_pretend_pas_avoir_efface(self):
        cle = FauxTransport(comptes=[b"A:x", b"B:y", b"C:z"],
                             echecs={b"B:y": 0x6985})
        with _muet() as sortie:
            rc = oath.cmd_delete(cle, _args("delete", "B:y"))
        self.assertEqual(rc, 1)
        texte = sortie.getvalue()
        self.assertNotIn("effacé", texte)
        # Et le magasin factice, qui simule fidèlement un refus de la carte,
        # n'a PAS bougé — le témoin positif du test précédent le confirme.
        self.assertIn(b"B:y", cle.comptes)


# --------------------------------------------------------------------------
# RENAME — l'INS, et les DEUX TAG_NAME dans le bon ordre
# --------------------------------------------------------------------------
class TestCablageRename(unittest.TestCase):
    def test_rename_porte_le_bon_ins_et_les_deux_noms_dans_l_ordre(self):
        # Encore un compte du milieu, pour la même raison que DELETE.
        cle = FauxTransport(comptes=[b"A:premier", b"B:milieu", b"C:dernier"])
        with _muet() as sortie:
            rc = oath.cmd_rename(cle, _args("rename", "B:milieu", "B:nouveau"))
        self.assertEqual(rc, 0)
        trame = cle.envoyees[-1]
        self.assertEqual(trame[1], oath.INS_RENAME)  # 05
        # L'ORDRE est la propriété qui compte : oath_do_rename() lit le
        # PREMIER 0x71 comme la cible. Les inverser renommerait un compte
        # inconnu, ou le compte en lui-même — sans erreur visible.
        self.assertEqual(
            oath.tlv_parse(trame[5:]),
            [(oath.TAG_NAME, b"B:milieu"), (oath.TAG_NAME, b"B:nouveau")])
        self.assertIn("B:nouveau", sortie.getvalue())
        # Témoin positif direct : le nom a VRAIMENT changé dans le magasin.
        self.assertEqual(cle.comptes,
                          [b"A:premier", b"B:nouveau", b"C:dernier"])

    def test_un_mot_d_etat_en_echec_ne_pretend_pas_avoir_renomme(self):
        cle = FauxTransport(comptes=[b"A:x", b"B:y"],
                             echecs={b"A:x": 0x6A80})  # nom déjà pris
        with _muet() as sortie:
            rc = oath.cmd_rename(cle, _args("rename", "A:x", "B:y"))
        self.assertEqual(rc, 1)
        self.assertNotIn("renommé", sortie.getvalue())
        # Le magasin factice n'a pas bougé — le témoin positif ci-dessus
        # prouve qu'il aurait bougé si la carte avait répondu 9000.
        self.assertEqual(cle.comptes, [b"A:x", b"B:y"])


# --------------------------------------------------------------------------
# I5 — le RESET n'envoie rien avant que l'hôte ait obtenu le mot exact
# --------------------------------------------------------------------------
class TestCablageReset(unittest.TestCase):
    def test_rien_ne_part_sans_le_mot_exact(self):
        """L'appui sur la clé est le SECOND rempart, pas le premier.

        Envoyer l'APDU d'abord ferait s'allumer l'écran de la clé alors que
        la décision n'est pas prise : il ne resterait qu'un geste réflexe
        entre Mae et seize secrets détruits.
        """
        for saisie in ("oui", "", "effacer", "EFFACER TOUT"):
            with self.subTest(saisie=saisie):
                cle = FauxTransport(comptes=[b"A:x", b"B:y"])
                with _entree(_Flux("", tty=True)), _saisie(saisie), _muet():
                    rc = oath.cmd_reset(cle, _args("reset"))
                self.assertEqual(rc, 1)
                self.assertNotIn(oath.INS_RESET, cle.ins_envoyees())

    def test_un_tuyau_ne_confirme_pas_un_reset(self):
        """« yes | niphar-oath reset » viderait la clé sans qu'on ait lu.

        L'entrée standard est ici un tuyau qui porte le mot exact, et `input`
        le rendrait : c'est bien le contrôle du terminal qui doit refuser.
        """
        cle = FauxTransport(comptes=[b"A:x", b"B:y"])
        with _entree(_Flux("EFFACER\n", tty=False)), \
                _saisie(oath.RESET_MOT), _muet():
            rc = oath.cmd_reset(cle, _args("reset"))
        self.assertEqual(rc, 1)
        self.assertNotIn(oath.INS_RESET, cle.ins_envoyees())

    def test_le_mot_exact_sur_un_terminal_envoie_le_reset(self):
        # Témoin positif : sans lui, les deux tests ci-dessus seraient verts
        # sur un client qui n'enverrait JAMAIS de RESET.
        cle = FauxTransport(comptes=[b"A:x", b"B:y"])
        with _entree(_Flux("", tty=True)), _saisie(oath.RESET_MOT), _muet():
            rc = oath.cmd_reset(cle, _args("reset"))
        self.assertEqual(rc, 0)
        self.assertIn(oath.apdu_reset(), cle.envoyees)

    def test_la_liste_de_ce_qui_va_mourir_est_affichee_avant_la_question(self):
        # Le nombre seul ne permet pas de reconnaître qu'on s'est trompé de
        # clé ; l'hôte a la place de nommer, l'écran de la clé ne l'a pas.
        cle = FauxTransport(comptes=[b"OVH:perso", b"GitHub:mae"])
        with _entree(_Flux("", tty=True)), _saisie("non"), _muet() as sortie:
            oath.cmd_reset(cle, _args("reset"))
        texte = sortie.getvalue()
        self.assertIn("OVH:perso", texte)
        self.assertIn("GitHub:mae", texte)

    def test_un_magasin_vide_n_envoie_rien_et_ne_demande_rien(self):
        cle = FauxTransport()

        def jamais(_=""):
            raise AssertionError("aucune question sur un magasin vide")

        with _entree(_Flux("", tty=True)), _saisie(jamais), _muet():
            rc = oath.cmd_reset(cle, _args("reset"))
        self.assertEqual(rc, 0)
        self.assertNotIn(oath.INS_RESET, cle.ins_envoyees())


if __name__ == "__main__":
    unittest.main(verbosity=1 if "-q" in sys.argv else 2)
