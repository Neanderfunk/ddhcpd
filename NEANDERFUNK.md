# Zweig `neanderfunk`

Dieser Fork von [freifunk-gluon/ddhcpd](https://github.com/freifunk-gluon/ddhcpd)
liefert den verteilten DHCP-Server für das Gluon-Paket `neanderfunk-ddhcpd`
im Feed [Neanderfunk/packages](https://github.com/Neanderfunk/packages) von
Freifunk Neanderland. Der Zweig `master` folgt unverändert dem Original,
unsere Änderungen liegen im Zweig `neanderfunk`, je Änderung ein Commit, auf
dem Upstream-Stand `f6724b5` (den auch `ffgraz-ddhcpd` in den
Gluon-community-packages baut).

| Commit | Was |
| --- | --- |
| node id aus der link-local-Adresse | `config.node_id` wurde nie gesetzt, alle Knoten liefen mit derselben (meist Null-)ID, die aber Blockkonflikte entscheidet. Jetzt die IID der link-local-Adresse des Server-Interfaces. Übernommen von [ffgraz/ddhcpd](https://github.com/ffgraz/ddhcpd) `8a1d03a` (Zweig `mkg-2`) |
| Kinder bei SIGCHLD alle abholen | ein `wait()` je Signal ließ Zombies stehen. Übernommen von ffgraz/ddhcpd `065cec3` |
| Bestehende Lease wieder anbieten, Option 50 | jedes DISCOVER belegte eine neue Adresse (im Labor 73 Adressen je Client in 2 Minuten). Jetzt zuerst die Lease desselben Clients (chaddr), dann die gewünschte Adresse, wenn frei und in einem eigenen Block |
| NAK für Leases anderer Clients | der Blockeigner bestätigte weitergeleitete REQUESTs nur nach Adresse; jetzt NAK, wenn die Lease einem anderen chaddr gehört, und der Client wird eingetragen |
| Eigene Länge der Lernphase | `-l SEKUNDEN`; ohne Angabe wie bisher `block_timeout` lang |
| Zusammenfassung für Maschinen | `ddhcpdctl -m`: je Zeile „Schlüssel Wert“, Blöcke (total = free + ours + claiming + foreign + releasing), Leases der eigenen Blöcke, mit `DDHCPD_STATISTICS` die Zähler für Roaming und DHCP (nie zurückgesetzt), je vergebener Lease eine Zeile `leased <mac>`. Für respondd und die Statusseite |
| Konflikt: mehr aktive Leases gewinnt | Claimen zwei Knoten denselben Block (nach Inselbetrieb, etwa beim Supernode-Schwenk), behält ihn, wer darin mehr aktive Leases hat; bei Gleichstand die node id wie bisher. Die Zahl steht im bisher reservierten Byte des Claims, kein neues Protokoll |
| Verlierer lässt auslaufen | neuer Blockzustand RELEASING: nichts Neues mehr aus dem Block, Anfragen seiner Clients gehen an den neuen Eigner, fertig wenn keine eigene Lease mehr aktiv ist, spätestens nach der Lease-Zeit |
| Verlierer übergibt seine Leases | je aktive Lease ein RENEWLEASE an den Gewinner (dieselbe Nachricht wie beim Roaming); der trägt sie ein (ACK) oder lehnt ab, wenn er die Adresse inzwischen anders vergeben hat (NAK, echte Doppelvergabe). Wiederholt, bis beantwortet |
| Zähler für Konflikte | gewonnen, verloren, Übergaben ACK/NAK; Blöcke im Auslaufen in `-m` |
| Keine Ausgabe je Paket auf stdout | `dhcp_packet_send` schrieb je Paket „Message LEN“ per printf; jetzt DEBUG |
| Sofort beenden auf SIGTERM/SIGINT | die Schleife wiederholte `epoll_wait` nach EINTR, ohne das Abbruch-Flag zu prüfen, und merkte das Signal erst nach dem nächsten Timeout (7.5 s); procd schickt nach 5 s SIGKILL, jeder Neustart dauerte so lange. Jetzt `epoll_pwait`, die Signale kommen nur dort an |
| Option 55 nur im Debug-Log | die Liste der angefragten Optionen ging je Paket ungefiltert nach stderr („1 3 6 ]“, unter procd als daemon.err); jetzt eine DEBUG-Zeile |

Ohne `-l` verhält sich der Zweig beim Start wie das Original. DISCOVER und
REQUEST verhalten sich bewusst anders (siehe Tabelle).

Logging: Der Logger schreibt nach stderr, sichtbar ist ab Werk nur WARNING
und schlimmer; mehr mit `-v` (mehrfach). Auf stdout kommen nur noch `-h` und
`-V`.

Lizenz wie das Original: GPL-3.0.

Aktualisieren auf einen neuen Upstream-Stand per Merge, nicht per Rebase:
das Gluon-Paket pinnt Commits dieses Zweigs, ein Rebase würde ihre IDs
ändern.

```bash
git fetch upstream
git merge upstream/master
```
