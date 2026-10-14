# OFM-WelcomeIPSip

OpenKNX-Modul für Busch-Welcome-IP-/ABB-Welcome-Türstationen **ohne SmartAP**, über ein
einmaliges Cloud-Pairing mit dem MyBuildings-Portal und danach rein lokales SIP zum Panel
(IP-Touch / Wi-Fi-Innenstation).

Das Modul meldet Klingel-Ereignisse der Außenstation(en) als KNX-Gruppenobjekte und kann die Tür
(und ein optionales Zweitschloss) über denselben Cloud-RPC-Aufruf öffnen, den auch die
Original-App nutzt (XMPP/Jabber-RPC per mTLS) – siehe [CHANGELOG](CHANGELOG.md). Die genaue
Wirkung ist bis zum ersten Test an echter Hardware nicht bestätigt.

## Features

- Einmaliges Pairing gegen das MyBuildings-Portal (Zertifikat, Integritätscode, ACL-Freigabe durch
  den Nutzer in der App)
- Kanäle, je Kanal eine Außenstation: Klingeln (DPT 1.001), Name der Station (DPT 16.001),
  Türöffner und Zweitschloss öffnen (je DPT 1.017, Eingang)
- Modulweit: Verbunden/SIP registriert (DPT 1.011), Diagnose (DPT 16.001), interner Ruf, Pairing
  per KO anstoßen
- Klingel-Erkennung läuft rein lokal über SIP zum Panel; der Türöffner braucht dagegen bei jedem
  Auslösen eine funktionierende Internet-/MyBuildings-Verbindung
- Konsolenbefehle `wis status | pair | unpair | doors | code | reregister | unlock`

## Voraussetzungen

- MyBuildings-Konto (Benutzername und Passwort der Welcome-/free@home-App) mit eingerichteter
  Anlage
- Internetzugang des KNX-Geräts (nur für das einmalige Pairing), danach lokale Netzwerkverbindung
  zum Panel
- Gerät mit **ESP32** (z. B. REG1-LAN-TP-Base). Auf RP2040-Geräten ist das Modul nicht enthalten.

Pairing und SIP-Protokoll sind nicht offiziell dokumentiert und per Reverse-Engineering ermittelt;
ABB kann sie jederzeit ändern. Vorbild ist
[rankjie/ha-abb-welcome](https://github.com/rankjie/ha-abb-welcome) für Home Assistant.

## Dokumentation

- [Applikationsbeschreibung](doc/Applikationsbeschreibung-WelcomeIPSip.md) – zugleich Quelle der
  ETS-Hilfetexte (`src/Baggages/Help_de`, erzeugt vom VS-Code-Task
  *OpenKNXproducer Documentation*)
- [CHANGELOG](CHANGELOG.md)

## Lizenz

GPL-3.0 – siehe [LICENSE](LICENSE)
