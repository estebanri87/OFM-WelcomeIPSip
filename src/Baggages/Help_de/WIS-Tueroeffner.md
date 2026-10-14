### Türöffner

Löst für diese Außenstation den Cloud-Aufruf aus, der in der Original-App das Entsperren der
Tür bewirkt (XMPP/Jabber-RPC, `RemoteInterface.unlockCloudContact`). Jedes empfangene
Telegramm löst den Vorgang aus, unabhängig vom Wert.

Anders als das Klingel-Objekt braucht der Türöffner immer eine funktionierende
Internet-/MyBuildings-Verbindung, auch wenn das lokale Panel erreichbar ist.

