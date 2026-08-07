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

- [x] Matériel conçu, revu, parti en fabrication (panneau Niphargus)
- [ ] Bring-up : boot, USB-Serial-JTAG, microSD
- [ ] MSC multi-ISO
- [x] Intégration OpenPGP CCID + CR-HMAC OTP-HID (specs KeSp) — PGP validé bout
      en bout sur matériel le 2026-08-07 (`gpg --card-status`, génération de
      clé, signature exigeant la confirmation physique — voir
      [`docs/HARDWARE.md`](docs/HARDWARE.md)) ; OTP-HID vérifié seulement à
      l'énumération USB, l'échange CR-HMAC réel reste à tester faute
      d'outillage HID sur le poste de dev
- [ ] Flash du C6 embarqué via esp-hosted (radio du P4, optionnel)
