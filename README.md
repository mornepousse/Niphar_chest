# Niphar_chest

Le **coffre** du clavier split [Niphargus](https://github.com/mornepousse/Niphargus) :
un ESP32-P4 embarqué dans la moitié gauche, derrière un hub USB, qui ne s'éveille
qu'en filaire. « Plein de choses, une à la fois » :

- **Clé USB multi-ISO** : la microSD expose des images bootables sélectionnables
  (MSC), la trousse de secours de l'adminsys.
- **Carte OpenPGP sur CCID** et **clé de sécurité CR-HMAC sur HID** — protocoles
  repris de [KeSp_firmware](https://github.com/mornepousse/KeSp_firmware)
  (`docs/OPENPGP_CARD.md`, `docs/SECURITY_KEY.md`). Pas de FIDO/CTAP : KeSp ne
  l'implémente pas non plus, c'est noté « Phase 3 » côté amont.
- **Stockage** amovible.

Matériel : voir [`docs/HARDWARE.md`](docs/HARDWARE.md) — contrat vérifié à la netlist
(revue Niphargus du 2026-08-06).

## État

Trois cartes, un seul firmware (voir [`docs/HARDWARE.md`](docs/HARDWARE.md)) :

- [x] **JC-ESP32P4-M3-DEV** (kit de dev) — matériel de développement, la carte
      qui tourne au quotidien.
- [x] **wt9932_key** (module WT9932P4-TINY) — clé de sécurité OpenPGP autonome,
      deux boutons et une LED en façade, validée sur matériel le 2026-08-17 par
      appui physique réel (voir [`docs/HARDWARE.md`](docs/HARDWARE.md)).
- [ ] **niphar_chest** (le coffre) — **non fabriqué**. Le firmware compile pour
      cette carte mais n'a jamais tourné sur sa cible.

- [x] **Écran OLED SSD1306** sur la carte-clé — la clé cesse d'être aveugle :
      elle annonce ce qu'on lui demande d'autoriser avant qu'on appuie. Écran
      d'accueil, quatre écrans d'état en police double hauteur, barre de
      décompte, veille à logo errant. Validé à l'œil sur la dalle le
      2026-08-17 ; l'écran de confirmation et sa barre restent à voir, ils
      exigent une vraie opération OpenPGP pour s'armer (voir
      [`docs/HARDWARE.md`](docs/HARDWARE.md)). **Défaut ouvert** : la carte
      bascule de mode spontanément, trois observations, non diagnostiqué.
- [ ] MSC multi-ISO
- [x] Intégration OpenPGP CCID + CR-HMAC OTP-HID (specs KeSp) — PGP validé bout
      en bout sur matériel le 2026-08-07 (`gpg --card-status`, génération de
      clé, signature exigeant la confirmation physique — voir
      [`docs/HARDWARE.md`](docs/HARDWARE.md)) ; OTP-HID vérifié seulement à
      l'énumération USB, l'échange CR-HMAC réel reste à tester faute
      d'outillage HID sur le poste de dev
- [ ] Flash du C6 embarqué via esp-hosted (radio du P4, optionnel)
- [x] **U2F sur CTAPHID** — INIT/PING/MSG câblés, `U2F_VERSION`,
      `U2F_REGISTER`/`AUTHENTICATE` validés sur matériel (WT9932P4-TINY) y
      compris via `fido2-cred -M` (libfido2, 124 captures), autotest crypto
      au démarrage (`PASS`) et marge de pile mesurée (3532/6144 o) ; **aucune
      signature réelle produite** — le bouton de confirmation en façade est
      électriquement ouvert (voir [`docs/HARDWARE.md`](docs/HARDWARE.md#fido2--u2f--2026-08-18)).
      `authenticatorGetInfo` (CTAP2) répond mais n'est pas annoncé aux
      clients CTAP2 (`CTAPHID_CAPFLAG_CBOR` retiré) : `makeCredential`/
      `getAssertion` restent à écrire, plan 2.
