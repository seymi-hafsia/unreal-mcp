# MCP Python Server

Serveur MCP en Python pour piloter l’Éditeur Unreal via **Protocol v1.1** (framed JSON).

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

## Protocol v1.1 (résumé)

* **Framing** : `uint32 length` + payload JSON UTF-8.
* **Handshake** : client → `{type:"handshake", protocolVersion:1.1, engineVersion, pluginVersion, sessionId, resumeToken}` ; serveur → `handshake/ack` (`resume`, `windowMax`, `resumeToken`).
* **Heartbeats** : `ping` / `pong` (idle 15 s ; timeout 60 s).
* **Erreurs** : `{ ok:false, error:{ code, message, details } }`.
* **Compat** : un client legacy reçoit `PROTOCOL_VERSION_MISMATCH` puis fermeture.

## Fiabilisation réseau

* **Idempotence** : chaque requête propage `requestId` = `idempotencyKey`; le serveur met en cache les réponses (TTL 10 min) dans `logs/dedup.jsonl`.
* **Reprise** : en cas de reconnexion, le handshake v1.1 relaie `resumeToken` et signale les reprises via l’événement `connection.resume`.
* **Backpressure** : la fenêtre maximale (`windowMax`) annoncée par l’éditeur est conservée pour limiter les commandes simultanées.
* **Logs DX** : événements `dedup.hit` (réponse rejouée) + métriques `tool_*` enrichies avec la cause (ok/erreur, latence).

## Sécurité & Enforcement

Le serveur Python applique les règles du policy loader (`MCP_POLICY_PATH`).

* **RBAC** : résout le `role` transmis par le plugin (`admin|dev|artist|read_only`). Les tools sont filtrés via patterns allow/deny (deny prioritaire) → `TOOL_DENIED` en cas d’accès refusé.
* **Rate limiting** : fenêtre 60 s (global + par tool). Dépassement → `RATE_LIMITED` + `retryAfterSec`.
* **Input limits** : taille du payload JSON (`request_size_kb`) et cardinalité max des listes (`array_items_max`). Dépassement → `REQUEST_TOO_LARGE` / `ARRAY_TOO_LARGE`.
* **Validation** : si un schéma `jsonschema` est enregistré (`security/schema_registry.py`), les params sont validés avant dispatch (`INVALID_PARAMS`).
* **Sandbox chemins** : toute valeur `path|dir|root` est normalisée (résolution `..`, symlinks, casse Windows) puis validée contre `paths.allowed/forbidden`. Hors périmètre → `PATH_NOT_ALLOWED`.
* **Audit HMAC** : les réponses mutantes incluent `security.auditSig` + `serverTs` + `nonce`. Le secret provient de `audit.hmac_secret_env` (`MCP_AUDIT_SECRET`).
* **Redaction** : toute clé/valeur contenant `token`, `password`, `secret`, `key` est redacted (`[REDACTED]`) avant signature et logging.

> Les audits signés se trouvent dans la réponse JSON (`security`). Conservez la même clé HMAC côté observabilité pour recalcule/verify.

## Observabilité

* **Logs JSONL** : `Python/logs/events.jsonl` (événements) & `metrics.jsonl` (métriques). Rotation automatique (20 MB, 3 fichiers).
* **Corrélation** : chaque requête inclut `requestId` et `meta.ts`/`meta.durMs` (client ↔ plugin ↔ serveur).
* **Métriques** : `tool_calls_total` (succès/erreur) et `tool_duration_ms` (durée). Exploitables en post-traitement (jq, etc.).
* **Tool `mcp.health`** : expose versions (serveur/protocole/python), uptime, clients actifs, flags `allowWrite`/`dryRun`, chemins autorisés, RTT best-effort et infos handshake plugin.

## Outils routés

Le serveur relaie les **tools** vers le plugin UE. Quelques exemples actuels :

* Lecture : `asset.find`, `asset.exists`, `asset.metadata`, `sc.status`
* Mutations : `sc.checkout`, `sc.add`, `sc.revert`, `sc.submit`
* Assets CRUD : `asset.create_folder`, `asset.rename`, `asset.delete`, `asset.fix_redirectors`, `asset.save_all`
* Assets Batch Import : `asset.batch_import` (FBX/Textures/Audio, presets/options, SCM)
* Actors (Editor) : `actor.spawn`, `actor.destroy`, `actor.attach`, `actor.transform`, `actor.tag`
  *(toutes les mutations respectent `allow_write`, `dry_run`, `allowed_paths` et nécessitent checkout/mark-for-add selon réglages)*
* Levels (Editor) : `level.save_open`, `level.load`, `level.unload`, `level.stream_sublevel`
  *(mutations de l’état des maps ouvertes : sauvegarde SCM, ouverture/streaming de sous-niveaux et DataLayers, transactions+audit)*
* Content Hygiene : `content.scan`, `content.validate`, `content.fix_missing`, `content.generate_thumbnails`
  *(scan/validate fonctionnent même en read-only ; `content.fix_missing` & `content.generate_thumbnails` respectent gates, transactions et SCM)*
* Sequencer : `sequence.create`, `sequence.bind_actors`, `sequence.unbind`, `sequence.list_bindings`, `sequence.add_tracks`, `sequence.export`
  *(création + mutations : bind/unbind/list, ajout de pistes transform/visibility/property/camera-cut ; export JSON/CSV read-only)*
* Materials : `mi.create`, `mi.set_params`, `mi.batch_apply`, `mesh.remap_material_slots`
  *(création/overrides de MI, assignation scène en masse, remap de slots StaticMesh ; `mi.batch_apply` modifie les maps ouvertes, `mesh.remap_material_slots` agit sur un asset)*
* Niagara (Editor) : `niagara.spawn_component`, `niagara.set_user_params`, `niagara.activate`, `niagara.deactivate`
  *(mutations scène côté Éditeur/PIE — pas d’édition structurelle des systèmes Niagara)*
* MetaSounds (préversion) : `metasound.spawn_component`, `metasound.set_params`, `metasound.play`, `metasound.stop`, `metasound.export_info`, `metasound.patch_preset`, `metasound.render_offline`
  *(routes déclarées côté serveur/éditeur mais actuellement stubs renvoyant `NOT_IMPLEMENTED`)*
* Navigation éditeur : `level.select`, `viewport.focus`, `camera.bookmark` (`persist=true` pour `set` ⇒ mutation, sinon lecture)

> `asset.batch_import` peut prendre plusieurs secondes (import FBX + textures). La réponse contient le détail par fichier (`created/skipped/overwritten`, warnings, audit).

## Build & Test

### Prérequis

* Accès en lecture/écriture au dossier projet (`Saved/Cooked`, `Saved/StagedBuilds`, `Saved/Logs`) et au dossier d’archive cible.
* `RunUAT.bat` (Windows) ou `RunUAT.sh` (macOS/Linux) présent sous `Engine/Build/BatchFiles` du moteur utilisé.
* Droits suffisants pour lancer des processus externes depuis le serveur Python.

### Variables d’environnement utiles

* `UE_ENGINE_ROOT` : utilisé si `engineRoot` n’est pas fourni dans les paramètres du tool.
* Les entrées `env{}` passées dans les paramètres (ex. `UE_SDKS_ROOT`, `ANDROID_HOME`, `NDKROOT`) sont fusionnées dans l’environnement du processus RunUAT.

### Exemple d’appel minimal

```json
{
  "tool": "uat.buildcookrun",
  "params": {
    "engineRoot": "D:/UE_5.6",
    "uproject": "D:/Proj/MyGame/MyGame.uproject",
    "platforms": ["Win64"],
    "cook": true,
    "stage": true,
    "package": true,
    "archive": true,
    "timeoutMinutes": 120
  }
}
```

`dryRun=true` renvoie uniquement la commande et les dossiers touchés, sans lancer RunUAT.

## MetaSounds (préversion)

Les routes MetaSound sont déjà enregistrées côté serveur (`metasound.*`, `metasound.render_offline`) mais renvoient pour l’instant `NOT_IMPLEMENTED`. Elles servent de point d’extension pour une future intégration (UnrealEditor-Cmd + Submix Recorder). La structure de payload suivra le cahier des charges défini dans la roadmap (sourcePath, params, durée, sampleRate, etc.).

## CLI locale (`mcp`)

Une CLI Typer accompagne le serveur pour exécuter des tools ou des pipelines sans agent externe.

### Installation

```bash
cd Python/cli
pip install -e .

# Vérifier l'installation
mcp --help
```

L’adresse du serveur peut être fournie via `--server` (par défaut `127.0.0.1:8765`) ou la variable `MCP_SERVER`.

### Exemples

```bash
# Exécuter un tool avec paramètres inline
mcp run asset.fix_redirectors --params-json '{"paths":["/Game/Core"],"recursive":true}'

# Lancer une recette YAML avec variables dynamiques
mcp recipe run ./pipelines/content_cleanup.yaml --vars GAME_ROOT=/Game/Core --parallel 3

# Valider et dry-run une recette (plan + audit consolidé)
mcp recipe test ./pipelines/content_cleanup.yaml --dry-run --output yaml
```

Options communes :

* `--dry-run` : ajoute `DryRun=true` aux params (si absent) et balise `meta.dryRun`.
* `--retry N` : retries transport (command run) ou par step (`retry.max_attempts`).
* `--parallel N` : exécute les steps indépendants en parallèle.
* `--vars key=val` & `--vars-file vars.yaml` : variables injectées dans `${VAR}` / `${{ steps.* }}`.
* `--select expr` : filtre la réponse via JMESPath (`result.savedCount`, `steps.export.audit`…).
* `--output json|yaml`, `--timeout`, `--log-level`, `--env KEY=VALUE`.

### Format des recettes

```yaml
version: 1
vars:
  GAME_ROOT: "/Game/Core"
steps:
  - name: save-open
    tool: level.save_open
    params:
      modifiedOnly: true

  - name: import-assets
    tool: asset.batch_import
    params:
      destPath: "${GAME_ROOT}/Props"
      files:
        - "D:/Imports/Chair.fbx"
      preset: "fbx_static"
    retry:
      max_attempts: 3
      backoff_sec: 2

  - name: export-seq
    tool: sequence.export
    needs: [import-assets]
    params:
      sequencePath: "${{ steps.import-assets.result.sequencePath }}"
      format: json
    save_as: out/seq.json
```

Le loader effectue une validation basique (version, steps, dépendances, fichiers). Chaque step supporte `when`, `needs`, `retry`, `timeout_sec`, `save_as`, `parallel_group`. Les sorties `audit.actions`/`diffs` sont fusionnées dans le résumé final.

### Structure de la réponse

* `ok`, `exitCode`, `durationSec`, `commandLine` : statut d’exécution et temps passé par plateforme.
* `logs.uatLog` et `logs.cookLog` (si trouvé) : chemins absolus vers les fichiers de log persistants.
* `artifacts[]` : chemins d’artefacts détectés (`*.exe`, `*.pak`, `*.apk`, `*.ipa`, etc.).
* `highlights[]` : extraits notables du log (cook terminé, pak/iostore, archive…).
* `warnings[]` : informations non bloquantes (ex. `ARTIFACTS_NOT_FOUND`).
* `results[]` : présent uniquement en cas de build multi-plateformes (une entrée par plateforme).

### Notes plateformes

* **Windows** : artefacts typiques dans `ArchiveDir/Windows/<Project>/WindowsNoEditor/` (EXE + .pak/.ucas/.utoc).
* **Android** : APK/OBB repérés dans `ArchiveDir/Android*/` selon le flavour configuré.
* **iOS/macOS** : packaging produit `.ipa`/`.app` dans les sous-dossiers correspondants.
* `pak`, `iostore`, `compressed`, `nodebuginfo`, `archiveDir` et autres options sont exposées un-à-un via les paramètres du tool.

## Automation

### Prérequis généraux

* `UnrealEditor-Cmd` doit être disponible sous `<engineRoot>/Engine/Binaries/<Platform>/` (même moteur que le projet).
* `RunUAT.bat`/`.sh` doit être exécutable depuis le serveur Python (identique à `uat.buildcookrun`).
* Les tests Gauntlet nécessitent un build déjà cooké/stagé (`build.path`) accessible depuis la machine serveur.

### `automation.run_specs`

* Lance les Automation Tests en mode Editor via `UnrealEditor-Cmd`.
* Paramètres principaux : `tests[]` (joker `*` accepté), `map`, `headless` (`-NullRHI`), `extraArgs[]`, `timeoutMinutes`.
* Logs : `logs/automation/automation_YYYY-MM-DD_HH-MM-SS.log` + export XML sous `logs/automation/reports/report_*/` (si généré).
* Résultats : `results.total/passed/failed/skipped` + `results.failures[]`. Un échec de test n’implique pas `ok=false` si le process retourne `0`.
* Erreurs structurées possibles : `ENGINE_NOT_FOUND`, `UPROJECT_NOT_FOUND`, `TIMEOUT`, `PROCESS_FAILED`, `REPORT_PARSE_FAILED`.

```json
{
  "tool": "automation.run_specs",
  "params": {
    "engineRoot": "D:/UE_5.6",
    "uproject": "D:/Proj/MyGame/MyGame.uproject",
    "tests": ["Project.Functional", "MySuite.*"],
    "headless": true,
    "timeoutMinutes": 30
  }
}
```

### `gauntlet.run`

* Déclenche `RunUAT Gauntlet` sur un build existant (`build.path`). Une seule plateforme par invocation.
* Paramètres clés : `test`, `platform`, `config` (client config), `extraArgs[]` (transmis tels quels), `timeoutMinutes`.
* Logs : `logs/gauntlet/gauntlet_uat_YYYY-MM-DD_HH-MM-SS.log` + tentative de détection du log Gauntlet (`logs.gauntletLog`).
* Résultats : `results.total/passed/failed` + `results.artifactsDir` si détecté. `ok` reflète l’exit code RunUAT (tests échoués ⇒ `results.failed>0`).
* Erreurs structurées : `ENGINE_NOT_FOUND`, `UPROJECT_NOT_FOUND`, `BUILD_PATH_NOT_FOUND`, `PLATFORM_UNSUPPORTED`, `PROCESS_FAILED`, `TIMEOUT`, `REPORT_PARSE_FAILED`.

```json
{
  "tool": "gauntlet.run",
  "params": {
    "engineRoot": "D:/UE_5.6",
    "uproject": "D:/Proj/MyGame/MyGame.uproject",
    "test": "MyGauntletSuite",
    "platform": "Win64",
    "config": "Development",
    "build": { "path": "D:/Builds/MyGame/Windows/MyGame/WindowsNoEditor" },
    "timeoutMinutes": 90
  }
}
```

> `ok` reflète le statut process (`exitCode == 0`). Inspectez `results.failed` pour distinguer les échecs de tests.

## Débogage

* Lancer avec `--verbose` (si option dispo) ou consulter les logs côté Éditeur (Saved/Logs).
* Tester le framing avec un client “echo” local si nécessaire.

## Roadmap (extraits)

* Packs à venir : Actors/Levels avancés, Sequencer avancé, BuildCookRun & Gauntlet.

