# Niphar_chest

Le **coffre** du clavier split [Niphargus](https://github.com/mornepousse/Niphargus) :
un ESP32-P4 embarqué dans la moitié gauche, derrière un hub USB, qui ne s'éveille
qu'en filaire. « Plein de choses, une à la fois » :

- **Clé USB multi-ISO** : la microSD expose des images bootables sélectionnables
  (MSC), la trousse de secours de l'adminsys.
- **Token PGP / clé de sécurité** — voir les protocoles déjà définis côté
  [KeSp_firmware](https://github.com/mornepousse/KeSp_firmware) (`docs/OPENPGP_CARD.md`,
  `docs/SECURITY_KEY.md`).
- **Stockage** amovible.

Matériel : voir [`docs/HARDWARE.md`](docs/HARDWARE.md) — contrat vérifié à la netlist
(revue Niphargus du 2026-08-06).

## État

- [x] Matériel conçu, revu, parti en fabrication (panneau Niphargus)
- [ ] Bring-up : boot, USB-Serial-JTAG, microSD
- [ ] MSC multi-ISO
- [ ] Intégration PGP/FIDO (specs KeSp)
- [ ] Flash du C6 embarqué via esp-hosted (radio du P4, optionnel)
