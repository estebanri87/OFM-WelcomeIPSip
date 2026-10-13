<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Applikationsbeschreibung WelcomeIP SIP (WelcomeIPSip)

Das Modul bindet eine Busch-Welcome-IP-/ABB-Welcome-Türstation **ohne SmartAP** an: einmaliges
Cloud-Pairing gegen das MyBuildings-Portal, danach rein lokales SIP zum Panel (IP-Touch /
Wi-Fi-Innenstation). Je Kanal eine Außenstation, deren Klingel-Ereignis als KNX-Gruppenobjekt
gemeldet wird.

Pairing-Parameter, Türöffner und die vollständigen Kommunikationsobjekte folgen in einem
späteren Entwicklungsschritt dieses Moduls.

---

## Inhaltsverzeichnis

- [ETS-Parameter](#kanaltyp)
  - [Kanaltyp](#kanaltyp)

---

# ETS-Parameter

<!-- DOC HelpContext="Kanaltyp" -->
## Kanaltyp

Wählt, ob dieser Kanal eine Außenstation der Busch-Welcome-IP-Anlage abbildet.

* **Deaktiviert** - Kanal ist inaktiv
* **Außenstation** - Kanal meldet Klingel-Ereignisse dieser Außenstation

<!-- DOCEND -->
