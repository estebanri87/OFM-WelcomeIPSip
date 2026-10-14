<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Applikationsbeschreibung WelcomeIP SIP (WelcomeIPSip)

Das Modul bindet eine Busch-Welcome-IP-/ABB-Welcome-Türstation **ohne SmartAP** an: einmaliges
Cloud-Pairing gegen das MyBuildings-Portal, danach rein lokales SIP zum Panel (IP-Touch /
Wi-Fi-Innenstation). Je Kanal eine Außenstation, deren Klingel-Ereignis als KNX-Gruppenobjekt
gemeldet wird.

Das Pairing selbst läuft nicht über die ETS, sondern über die Gerätekonsole (`wis pair`,
`wis status`, `wis code`, `wis doors`, `wis unpair`) oder das Objekt "Pairing starten". Die
ETS-Parameter dieser Seite legen nur die dafür nötigen Zugangsdaten und die Kanäle fest.

Türöffner und Zweitschloss lösen den Cloud-RPC-Aufruf aus, den auch die Original-App für das
Entsperren nutzt (XMPP/Jabber-RPC per mTLS). Die genaue Wirkung ist bis zum ersten Test an
echter Hardware nicht bestätigt (siehe [CHANGELOG](../CHANGELOG.md)).

---

## Inhaltsverzeichnis

- [ETS-Parameter](#zugangsdaten)
  - [Zugangsdaten](#zugangsdaten)
  - [Client-Name](#client-name)
  - [App-Typ](#app-typ)
  - [Gateway-UUID](#gateway-uuid)
  - [Panel-Adresse](#panel-adresse)
  - [Klingelsignal](#klingelsignal)
  - [Objekte](#objekte)
  - [Kanaltyp](#kanaltyp)
  - [Stationsnummer](#stationsnummer)
  - [Türöffner](#türöffner)
  - [Zweitschloss öffnen](#zweitschloss-öffnen)

---

# ETS-Parameter

<!-- DOC HelpContext="Allgemein" -->
## Allgemein

Das Modul bindet eine Busch-Welcome-IP-Anlage ohne SmartAP per einmaligem Cloud-Pairing und
danach rein lokalem SIP ein. Das Pairing selbst läuft über die Gerätekonsole, nicht über die ETS.

<!-- DOCEND -->

<!-- DOC HelpContext="Zugangsdaten" -->
## Zugangsdaten

Benutzername und Passwort des MyBuildings-Kontos (dieselben wie in der Welcome- bzw.
free@home-next-App). Werden nur für das einmalige Pairing benötigt und danach nicht mehr
abgefragt. Liegen im ETS-Projekt im Klartext – entsprechend behandeln.

<!-- DOCEND -->

<!-- DOC HelpContext="Client-Name" -->
## Client-Name

Name, unter dem sich dieses Gerät im MyBuildings-Portal einträgt. Erscheint dort in der
Geräteliste. Eine Änderung nach einem bereits abgeschlossenen Pairing löst kein neues Pairing
aus – dafür `wis pair` an der Konsole bzw. das Objekt "Pairing starten" verwenden.

<!-- DOCEND -->

<!-- DOC HelpContext="App-Typ" -->
## App-Typ

Als welcher Client-Typ sich das Gerät beim Pairing im Portal meldet:

* **free@home next** - Standard. Erscheint im Portal wie das Handy, mit dem die Tür heute
  funktioniert. Nicht in jedem Fall erprobt: Lehnt das Portal diesen Typ ab, oder bleibt die
  Freigabe aus (Diagnose "wartet auf Freigabe" ohne Fortschritt), auf "Welcome" umstellen.
* **Welcome** - erprobt, erscheint im Portal als "ABB-WELCOME IP".

Ein Wechsel erzeugt beim nächsten Pairing ein neues Zertifikat und einen neuen Portal-Eintrag.

<!-- DOCEND -->

<!-- DOC HelpContext="Gateway-UUID" -->
## Gateway-UUID (optional)

In aller Regel leer lassen: Das Gateway wird beim Pairing automatisch über das Portal gefunden.
Nur ausfüllen, wenn diese automatische Suche fehlschlägt oder mehrere Gateways am selben
MyBuildings-Konto hängen und das falsche gefunden wird.

<!-- DOCEND -->

<!-- DOC HelpContext="Panel-Adresse" -->
## Panel-Adresse

IP-Adresse oder Hostname des Panels (IP-Touch / Innenstation) im lokalen Netz, auf dem Port 5061
(SIP über TLS) erreichbar. Eine feste Adresse bzw. eine DHCP-Reservierung wird empfohlen. Wird bei
jedem Reconnect neu aufgelöst; eine Änderung löst kein neues Pairing aus.

<!-- DOCEND -->

<!-- DOC HelpContext="Klingelsignal" -->
## Klingelsignal

Wie das Objekt "Klingeln" eines Kanals bei einem Ruf gesetzt wird:

* **Impuls** - kurzer Impuls beim Klingeln, unabhängig davon, wie lange der Anruf dauert
* **Dauer während des Klingelns** - bleibt gesetzt, solange der Anruf ansteht, und fällt ab,
  wenn er angenommen, abgebrochen oder beendet wird

<!-- DOCEND -->

<!-- DOC HelpContext="Objekte" -->
## Objekte

Einzelne Objekte dieser Seite bzw. der Kanäle lassen sich abwählen, wenn sie nicht gebraucht
werden:

* **Name der Station** - Objekt je Kanal, das den aus der Türliste bekannten Namen der
  Außenstation sendet
* **Interner Ruf** - modulweites Objekt, das gesetzt wird, wenn eine Innenstation oder
  Wachzentrale ruft (kein Ruf von einer der konfigurierten Außenstationen)

<!-- DOCEND -->

<!-- DOC HelpContext="Kanaltyp" -->
## Kanaltyp

Wählt, ob dieser Kanal eine Außenstation der Busch-Welcome-IP-Anlage abbildet.

* **Deaktiviert** - Kanal ist inaktiv
* **Außenstation** - Kanal meldet Klingel-Ereignisse dieser Außenstation

<!-- DOCEND -->

<!-- DOC HelpContext="Stationsnummer" -->
## Stationsnummer

Die Außenstation dieses Kanals, als Position in der Türliste, die das Pairing aus der Anlage
ausliest (1 = erste Station). Die Konsole zeigt diese Liste mit Nummer, Namen und `station_id`
über den Befehl `wis doors`. Passt die Nummer nicht mehr zu einer vorhandenen Station (z. B. nach
einer Änderung in der Anlage), meldet das Modul das als Diagnose und im Log.

<!-- DOCEND -->

<!-- DOC HelpContext="Tueroeffner" -->
## Türöffner

Löst für diese Außenstation den Cloud-Aufruf aus, der in der Original-App das Entsperren der
Tür bewirkt (XMPP/Jabber-RPC, `RemoteInterface.unlockCloudContact`). Jedes empfangene
Telegramm löst den Vorgang aus, unabhängig vom Wert.

Anders als das Klingel-Objekt braucht der Türöffner immer eine funktionierende
Internet-/MyBuildings-Verbindung, auch wenn das lokale Panel erreichbar ist.

<!-- DOCEND -->

<!-- DOC HelpContext="Zweitschloss" -->
## Zweitschloss öffnen

Wie "Türöffner", aber für ein zweites Schloss oder Tor an derselben Außenstation. Nur
Stationen mit einem konfigurierten Zweitschloss reagieren darauf; bei allen anderen hat
dieses Objekt keine Wirkung (im Log erscheint dann ein Hinweis).

<!-- DOCEND -->
