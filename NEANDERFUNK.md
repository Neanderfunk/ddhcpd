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

Ohne `-l` verhält sich der Zweig beim Start wie das Original. DISCOVER und
REQUEST verhalten sich bewusst anders (siehe Tabelle).

Logging: Der Logger schreibt nach stderr, sichtbar ist ab Werk nur WARNING
und schlimmer; mehr mit `-v` (mehrfach).

Lizenz wie das Original: GPL-3.0.

Aktualisieren auf einen neuen Upstream-Stand per Merge, nicht per Rebase:
das Gluon-Paket pinnt Commits dieses Zweigs, ein Rebase würde ihre IDs
ändern.

```bash
git fetch upstream
git merge upstream/master
```
