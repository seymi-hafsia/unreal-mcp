# MCP Python Server

Serveur MCP en Python pour piloter l’Éditeur Unreal via **Protocol v1** (framed JSON).

## Installation

```bash
cd Python
python -m venv .venv
# Windows: .venv\Scripts\activate
# macOS/Linux:
source .venv/bin/activate

pip install -r requirements.txt  # si présent (sinon, stdlib uniquement)
```

> Le serveur n’utilise que la **stdlib** (`socket`, `struct`, `json`, `argparse`, `selectors`, `time`) — aucun package externe requis par défaut.

## Lancement

```bash
python server.py \
  --host 127.0.0.1 \
  --port 12029 \
  --allow-write 0 \
  --dry-run 1 \
  --allowed-path "/Game/Core" \
  --allowed-path "/Game/Art"
```

Variables d’environnement supportées :

* `MCP_ALLOW_WRITE=0|1`
* `MCP_DRY_RUN=0|1`
* `MCP_ALLOWED_PATHS=/Game/Core;/Game/Art`

## Protocol v1 (résumé)

* **Framing** : `uint32 length` + payload JSON UTF-8.
* **Handshake** : client → `{type:"handshake", protocolVersion:1, engineVersion, pluginVersion, sessionId}` ; serveur → `handshake/ack`.
* **Heartbeats** : `ping` / `pong` (idle 15 s ; timeout 60 s).
* **Erreurs** : `{ ok:false, error:{ code, message, details } }`.
* **Compat** : un client legacy reçoit `PROTOCOL_VERSION_MISMATCH` puis fermeture.

## Sécurité & Enforcement

* Le serveur annonce ses **capabilities/enforcement** (allowWrite/dryRun/allowedPaths).
* Si une requête indique `meta.mutation=true` et `allowWrite=0` → réponse immédiate `WRITE_NOT_ALLOWED` (le plugin ne tente rien).
* Un **audit** JSONL est écrit dans `logs/audit.jsonl` (timestamp, tool, mutation, dryRun, digest params, result.ok).

## Outils routés

Le serveur relaie les **tools** vers le plugin UE. Quelques exemples actuels :

* Lecture : `asset.find`, `asset.exists`, `asset.metadata`, `sc.status`
* Mutations : `sc.checkout`, `sc.add`, `sc.revert`, `sc.submit`
  *(soumis à allowWrite/dryRun/allowedPaths)*

## Débogage

* Lancer avec `--verbose` (si option dispo) ou consulter les logs côté Éditeur (Saved/Logs).
* Tester le framing avec un client “echo” local si nécessaire.

## Roadmap (extraits)

* Packs à venir : Actors/Levels, Sequencer avancé, Material Instances set, Niagara spawn & params, BuildCookRun & Gauntlet.

