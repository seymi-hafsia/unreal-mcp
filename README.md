# Unreal MCP — Remote control for Unreal Editor via MCP (UE 5.6)

> Contrôlez l’Éditeur Unreal depuis un serveur MCP (Python) et des agents externes.
> Version compatible **UE 5.6**. Réseau fiabilisé avec **Protocol v1** (framed JSON + handshake + ping/pong).

## ✨ Fonctionnalités actuelles
- **Protocol v1** : framing binaire (uint32 + JSON), handshake versionné, heartbeats, schéma d’erreurs.
- **Sécurité** : gates **AllowWrite / DryRun / AllowedContentRoots / Tool allow/deny-list** + audit JSON.
- **Transactions & Undo** : toutes les mutations sont encapsulées dans des transactions éditeur.
- **Source Control intégré** : `sc.status / sc.checkout / sc.add / sc.revert / sc.submit` (provider-agnostic).
- **Assets v1 (lecture)** : `asset.find / asset.exists / asset.metadata` via Asset Registry.
- **Assets v2 (CRUD)** : `asset.create_folder / asset.rename / asset.delete / asset.fix_redirectors / asset.save_all`.
- **Assets v3 (Batch Import)** : `asset.batch_import` pour importer FBX/Textures/Audio avec presets, options et SCM.
- **Sequencer v1** : `sequence.create` pour générer un Level Sequence (fps, durée, évaluation, caméra/camera-cut/bind optionnels).
- **Materials v1 (Material Instances)** : `mi.create` et `mi.set_params` pour créer des MI et régler leurs overrides.
- **Niagara v1 (Editor)** : `niagara.spawn_component / niagara.set_user_params / niagara.activate / niagara.deactivate`.
- **Actors v1 (Editor)** : `actor.spawn / actor.destroy / actor.attach / actor.transform / actor.tag` (transactions, sélection, audit).
- **Camera helpers (Editor)** : `level.select / viewport.focus / camera.bookmark` (navigation + bookmarks, session & persistance).
- **Build & Test v2** : wrappers RunUAT `BuildCookRun`, `automation.run_specs` (Editor-Cmd) et `gauntlet.run` (cooked) avec logs persistants & parsing basique.
- **Settings Plugin** : Project Settings → **Plugins → Unreal MCP** (Network, Security, SCM, Logging, Diagnostics).

## 🛡️ Networking Reliability (Protocol v1.1)
- **Handshake v1.1** : reprise de session (`resumeToken`) et exposition de la fenêtre de backpressure (`windowMax`).
- **Idempotence** : chaque requête transporte désormais un `idempotencyKey` (dérivé du `requestId`).
- **Dedup JSONL** : le serveur Python conserve les réponses pendant 10 min (`logs/dedup.jsonl`) pour rejouer exactement une fois.
- **Logs DX** : événements structurés `connection.resume` et `dedup.hit` pour suivre les reconnections et déduplications.

## 🔧 Installation rapide

1. **Cloner** ce repo (ou votre fork) et ouvrir le projet UE.
2. **Activer le plugin** *Unreal MCP* dans l’éditeur si nécessaire.
3. **Vérifier UE 5.6** : le `.uproject` utilise `"EngineAssociation": "5.6"`.
4. **Configurer** le serveur Python (voir `Python/README.md`) et lancer le serveur MCP.
5. Dans UE : **Project Settings → Plugins → Unreal MCP**  
   - `ServerHost=127.0.0.1`, `ServerPort=12029` (par défaut)  
   - (optionnel) activer `bAutoConnectOnEditorStartup`
6. **Diagnostics** : depuis la page de réglages, cliquez **Test Connection** puis **Send Ping**.

## ⚙️ Réglages plugin (résumé)
- **Network** : host/port, timeouts, heartbeats, auto-connect.
- **Security** : `AllowWrite`, `DryRun`, `RequireCheckout`, `AllowedContentRoots`, `AllowedTools/DeniedTools`.
- **Source Control** : `EnableSourceControl`, `AutoConnectSourceControl`, `PreferredProvider`.
- **Logging** : niveau verbose protocole, dossier de logs.
- **Diagnostics** : boutons `Test Connection`, `Send Ping`, `Open Logs Folder`.

> Par défaut, **AllowWrite=false** et **DryRun=true** → aucune écriture n’est effectuée tant que vous n’avez pas explicitement autorisé.

## 🔐 Security & Hardening

La pile MCP implémente désormais une couche de défense en profondeur inspirée du protocole v1.1 :

- **RBAC** : rôles `admin`, `dev`, `artist`, `read_only` résolus côté plugin et serveur. Chaque rôle mappe sur des patterns allow/deny (`asset.*`, `sequence.*`, `!uat.*`…). Les denies (`!pattern`) sont prioritaires.
- **Allow/Deny patterns** : configuration unifiée (plugin + serveur YAML) pour les tools, avec support des glob `*`/`?`. Les racines de contenu autorisées/forbidden sont normalisées (`..`, symlinks, casse Windows) avant toute mutation.
- **Rate limiting** : fenêtre glissante 60 s avec quota global & par tool (ex : 120/min global, 30/min/tool). Les dépassements retournent `RATE_LIMITED` + `retryAfterSec`.
- **Input limits** : taille JSON (KB), profondeur/listes (hard cap `array_items_max`) + validation `jsonschema` pour les outils critiques (`asset.batch_import`, `sequence.create`, `mi.set_params`, …).
- **Audit signé** : chaque mutation renvoie un bloc `security` (`auditSig`, `serverTs`, `nonce`) signé HMAC-SHA256 (`MCP_AUDIT_SECRET`). Le client peut vérifier ou refuser (`RequireAuditSignature`).
- **Redaction** : les paramètres/résultats contenant `token`, `password`, `secret`, `key` sont remplacés par `[REDACTED]` dans les payloads de signature/logs.
- **Sandbox chemins** : toutes les entrées `path/dir/root` passent par une canonicalisation stricte. Les racines interdites l’emportent, sinon `PATH_NOT_ALLOWED` est renvoyé.
- **Nouveaux codes d’erreur** : `TOOL_DENIED`, `PATH_NOT_ALLOWED`, `RATE_LIMITED`, `REQUEST_TOO_LARGE`, `ARRAY_TOO_LARGE`, `INVALID_PARAMS` (schema), `CONFIRMATION_REQUIRED` (SCM soft guard côté UE).

### Configuration

**Plugin UE (`Project Settings → Plugins → Unreal MCP`)**

- `AllowedTools` accepte désormais des patterns globs avec `!pattern` pour les denies prioritaires.
- `ForbiddenContentRoots` complète `AllowedContentRoots` pour bloquer des sous-arbres (`/Game/Restricted`, `/Engine`, …).
- `Role` (`admin|dev|artist|read_only`) sélectionne le profil RBAC appliqué lors des handshakes.
- Limites client (`MaxRequestSizeKB`, `MaxArrayItems`) protègent la pipeline avant d’envoyer le frame JSON.
- `RateLimitPerMin` (global + per-tool) évite de saturer le serveur avant transport.
- `RequireAuditSignature` force la vérification des réponses mutantes (`UNVERIFIED_AUDIT` si la signature est manquante/incorrecte).

**Serveur Python (`Python/security/policy.yaml`)**

- Variable d’environnement `MCP_POLICY_PATH` → YAML décrivant `roles`, `limits`, `paths`, `audit`.
- `audit.hmac_secret_env` indique la variable d’environnement contenant le secret (ex : `MCP_AUDIT_SECRET`).
- Exemple :

```yaml
roles:
  dev:
    allow: ["asset.*", "sequence.*", "level.*", "content.*"]
    deny: ["security.*"]
  read_only:
    allow: ["sequence.export", "content.scan", "mcp.health"]
    deny: ["*.**"]
limits:
  rate_per_minute_global: 120
  rate_per_minute_per_tool: 30
  request_size_kb: 512
  array_items_max: 10000
paths:
  allowed: ["/Game/Core", "/Game/Art"]
  forbidden: ["/Game/Restricted", "/Engine"]
audit:
  require_signature: true
  hmac_secret_env: MCP_AUDIT_SECRET
```

Placez le secret HMAC dans un fichier `.env` (permissions 600) ou dans l’environnement du service systemd (`Environment=MCP_AUDIT_SECRET=...`).

## 📈 Observability & DX
- **Logs structurés JSONL** : côté plugin `Saved/Logs/UnrealMCP_events.jsonl` & `UnrealMCP_metrics.jsonl`; côté serveur Python `Python/logs/events.jsonl` & `metrics.jsonl` avec rotation.
- **Corrélation par `requestId`** : chaque tool embarque `meta.requestId`, timestamps (`ts`) et durée (`durMs`) dans les réponses et dans les logs.
- **Métriques légères** : incréments `tool_calls_total` et durées `tool_duration_ms` (par tool, succès/erreur).
- **Outils de santé** : tool read-only `mcp.health` retourne versions, uptime, flags d’enforcement, RTT et infos plugin.
- **Réglages** : nouveau `LogLevel`, `EnableJsonLogs` et boutons Diagnostics (ouvrir events/metrics log, tail live).

## 🖥️ CLI locale (`mcp`)
Un binaire `mcp` (Typer) est fourni pour piloter le serveur MCP sans passer par un agent externe.

```bash
# Lancer un tool directement
mcp run level.save_open --params-json '{"modifiedOnly": true}'

# Exécuter une recette YAML (pipeline DAG)
mcp recipe run ./.mcp/pipelines/content_cleanup.yaml --vars GAME_ROOT=/Game/Core

# Valider / dry-run une recette
mcp recipe test ./.mcp/pipelines/content_cleanup.yaml --dry-run
```

| Commande          | Description                                      |
|-------------------|--------------------------------------------------|
| `mcp run`         | Exécute un tool MCP unique avec paramètres JSON  |
| `mcp recipe run`  | Exécute un pipeline YAML (steps dépendants, DAG) |
| `mcp recipe test` | Valide la recette, affiche le plan, option dry-run |

Fonctionnalités transverses : `--dry-run`, `--retry`, `--parallel`, `--vars`/`--vars-file`, `--select` (JMESPath), `--output json|yaml`, `--timeout`, `--log-level`, `--server`.

## 🧰 Outils exposés (MCP Tools)

### Lecture (toujours autorisées)
| Tool              | Description                                 | Params clés                                      |
|-------------------|---------------------------------------------|--------------------------------------------------|
| `asset.find`      | Recherche d’assets via Asset Registry       | `paths[]`, `classNames[]`, `nameContains`, `limit/offset` |
| `asset.exists`    | Existence + classe d’un asset               | `objectPath`                                     |
| `asset.metadata`  | Métadonnées (class, tags, deps, size, …)    | `objectPath`                                     |
| `sc.status`       | Statut SCM par fichier/asset                | `assets[]` ou `files[]`                          |

### Mutations (soumis aux gates & transactions)
| Tool          | Description                   | Remarques |
|---------------|-------------------------------|-----------|
| `sc.checkout` | Checkout d’un lot de fichiers | No-op possible selon provider (Git) |
| `sc.add`      | Marquer pour ajout            |           |
| `sc.revert`   | Revert local                  |           |
| `sc.submit`   | Submit/commit avec message    |           |

| Tool                    | Description                           | Notes                                                  |
|-------------------------|---------------------------------------|--------------------------------------------------------|
| `asset.create_folder`   | Créer un dossier `/Game/...`          | Respecte `AllowedContentRoots`                         |
| `asset.rename`          | Renommer/déplacer un asset (package)  | Crée un redirector (corrigez via `asset.fix_redirectors`) |
| `asset.delete`          | Supprimer un ou plusieurs assets      | `force=false` bloque si référencé                      |
| `asset.fix_redirectors` | Corriger les redirectors dans un path | Utilise `AssetTools`, compatible récursif              |
| `asset.save_all`        | Sauvegarder assets modifiés           | Scope global ou par `paths[]`, `modifiedOnly` optionnel |
| `asset.batch_import`    | Importer un lot de fichiers           | Presets FBX/Textures/Audio, dry-run, SCM, conflits      |

#### Maps & Levels

| Tool                    | Description                                       | Notes                                |
|-------------------------|---------------------------------------------------|--------------------------------------|
| `level.save_open`       | Sauvegarde les maps ouvertes (persistante + sublevels chargés) | `modifiedOnly`, SCM/checkout respecté |
| `level.load`            | Ouvre une map persistante dans l’éditeur          | Option `loadSublevels` (`none/all/byNames`) |
| `level.unload`          | Décharge des sous-niveaux/World Partition layers  | `allowMissing` pour ignorer les absents |
| `level.stream_sublevel` | Charge/Décharge un streaming level ou DataLayer   | `blockUntilVisible`, support DataLayers |

```jsonc
// Exemple : level.load
{
  "mapPath": "/Game/Maps/Persistent.Persistent",
  "loadSublevels": "byNames",
  "sublevels": ["Gameplay", "Lighting"],
  "makeCurrent": true,
  "discardUnsaved": false
}
```

### Content Hygiene

| Tool                          | Type      | Description                                               |
|-------------------------------|-----------|-----------------------------------------------------------|
| `content.scan`                | read-only | Scan des refs cassées, assets manquants, redirectors, etc |
| `content.validate`            | read-only | Vérifie naming, tailles textures, LODs, collisions, MI    |
| `content.fix_missing`         | mutant    | Fix redirectors + remap soft refs + purge redirectors     |
| `content.generate_thumbnails` | mutant    | Régénère les miniatures d’un lot d’assets                 |

```jsonc
// Exemple : content.scan
{
  "paths": ["/Game/Core", "/Game/Art"],
  "recursive": true,
  "includeUnusedTextures": true,
  "includeReferencers": true
}
```

```jsonc
// Exemple : content.fix_missing
{
  "paths": ["/Game/Core"],
  "recursive": true,
  "fix": {
    "redirectors": true,
    "remapReferences": true,
    "deleteStaleRedirectors": true
  },
  "save": true
}
```

#### Materials

| Tool                        | Description                                                   | Notes |
|-----------------------------|----------------------------------------------------------------|-------|
| `mi.create`                 | Crée une Material Instance enfant d'un Material ou d'une MI    | Respecte `AllowedContentRoots`, overwrite option, SCM |
| `mi.set_params`             | Applique des overrides Scalar/Vector/Texture/StaticSwitch sur une MI existante | Support `clearUnset`, sauvegarde, audit détaillé |
| `mi.batch_apply`            | Assigne des Material Instances à des acteurs/composants par slot (index ou nom) | Scène uniquement (maps), Undo/Redo, pas de sauvegarde auto |
| `mesh.remap_material_slots` | Renomme/réordonne les slots d'un StaticMesh (duplication optionnelle) | Option de rebind des acteurs, SCM + sauvegarde optionnelle |

```jsonc
// Exemple : mi.set_params
{
  "miObjectPath": "/Game/Materials/Instances/MI_Master_Red.MI_Master_Red",
  "scalars": { "Roughness": 0.35, "Metallic": 1.0 },
  "vectors": { "BaseColor": [1.0, 0.1, 0.1, 1.0] },
  "textures": { "Albedo": "/Game/Art/Textures/T_Orc_Diffuse.T_Orc_Diffuse" },
  "switches": { "UseDetail": true },
  "clearUnset": false,
  "save": true
}
```

```jsonc
// Exemple : mi.batch_apply (deux acteurs)
{
  "targets": [
    {
      "actorPath": "/Game/Maps/Demo.Demo:PersistentLevel.SM_Statue_1",
      "assign": [
        { "slot": 0, "mi": "/Game/Materials/Instances/MI_Stone.MI_Stone" },
        { "slot": "Eyes", "mi": "/Game/Materials/Instances/MI_Gold.MI_Gold" }
      ]
    },
    {
      "actorPath": "/Game/Maps/Demo.Demo:PersistentLevel.SK_Orc_1",
      "component": "Mesh",
      "assign": [
        { "slot": "Body", "mi": "/Game/Characters/Materials/MI_OrcSkin.MI_OrcSkin" }
      ]
    }
  ]
}
```

#### Niagara (Editor)

| Tool                      | Description                                                       | Notes                                                          |
|---------------------------|-------------------------------------------------------------------|----------------------------------------------------------------|
| `niagara.spawn_component` | Instancie un `UNiagaraComponent` à partir d’un `UNiagaraSystem`   | Attach à un acteur existant ou spawn libre, `autoActivate`, sélection optionnelle |
| `niagara.set_user_params` | Définit des User Parameters typés (float, bool, int, vector, color, texture/mesh…) | Mutant : transaction + audit, support `saveActor`              |
| `niagara.activate`        | Active un composant Niagara (option `reset`)                      | Respecte Undo/Redo, audit                                      |
| `niagara.deactivate`      | Désactive un composant Niagara (option `immediate`)               | `DeactivateImmediate` si demandé, audit                        |

```jsonc
// Exemple : niagara.spawn_component
{
  "systemPath": "/Game/VFX/Systems/NS_Fire.NS_Fire",
  "attach": {
    "actorPath": "/Game/Maps/UEDPIE_0_Map.Map:PersistentLevel.SM_Statue_1",
    "socketName": "FX_Socket",
    "keepWorldTransform": true
  },
  "autoActivate": true,
  "initialUserParams": {
    "Float:Intensity": 2.5,
    "Vector:Tint": [1.0, 0.5, 0.2, 1.0],
    "Bool:Loop": true
  },
  "select": true
}
```

#### Actors

| Tool             | Description                          | Notes                                                   |
|------------------|--------------------------------------|---------------------------------------------------------|
| `actor.spawn`    | Instancier un acteur (classe ou BP)  | Transform + tags optionnels, sélection possible         |
| `actor.destroy`  | Détruire un ou plusieurs acteurs     | `allowMissing=true` ignore les références absentes      |
| `actor.attach`   | Attacher un acteur à un parent       | Supporte `keepWorldTransform`, `socketName`, weld       |
| `actor.transform`| Appliquer set/add sur location/rot/scale | `set` absolu puis `add` (delta)                        |
| `actor.tag`      | Ajouter/retirer/remplacer des tags   | `replace` (array ou `null`), `add`, `remove`            |

#### Sequencer

| Tool                     | Description                                      | Notes                                                  |
|--------------------------|--------------------------------------------------|--------------------------------------------------------|
| `sequence.create`        | Crée un Level Sequence (fps, durée, eval)        | Caméra Cine + CameraCut optionnels ; bind d'acteurs existants |
| `sequence.bind_actors`   | Lier un ou plusieurs acteurs à un Sequence       | Options `skipIfAlreadyBound`, `overwriteIfExists`, `labelPrefix`, `save` |
| `sequence.unbind`        | Retirer des bindings par GUID ou acteur          | Mutant ; support `save`, audit détaillé                |
| `sequence.list_bindings` | Lister les bindings existants d'un Sequence      | Read-only ; renvoie GUID, label, acteur courant        |
| `sequence.add_tracks`    | Ajouter des pistes (Transform/Visibility/Property) + cuts caméra | Mutant ; overwrite/dryRun/save, SCM + audit |
| `sequence.export`        | Exporter un Sequence en JSON/CSV (bindings, pistes, clés)        | Read-only ; filtres `include.*`, `frameRange`, CSV aplati |

```jsonc
// Exemple : sequence.create minimal
{
  "sequencePath": "/Game/Cinematics/Seq/SEQ_Intro",
  "displayRate": [24, 1],
  "durationFrames": 240,
  "evaluationType": "WithSubFrames"
}
```

```jsonc
// Exemple : sequence.bind_actors
{
  "sequencePath": "/Game/Cinematics/Seq/SEQ_Intro.SEQ_Intro",
  "actorPaths": [
    "/Game/Maps/UEDPIE_0_Map.Map:PersistentLevel.BP_TrainingDummy_C_2",
    "/Game/Maps/UEDPIE_0_Map.Map:PersistentLevel.CineCam_Intro"
  ],
  "skipIfAlreadyBound": true,
  "labelPrefix": "Dummy_",
  "overwriteIfExists": false,
  "save": true
}
```

```jsonc
// Exemple : sequence.add_tracks (transform + cuts)
{
  "sequencePath": "/Game/Cinematics/Seq/SEQ_Intro.SEQ_Intro",
  "bindings": [
    {
      "actorPath": "/Game/Maps/Map:PersistentLevel.BP_TrainingDummy_C_2",
      "tracks": [
        {
          "type": "Transform",
          "keys": [
            { "frame": 0, "location": [0, 0, 0] },
            { "frame": 120, "location": [0, 300, 0], "rotation": [0, 90, 0] }
          ]
        }
      ]
    }
  ],
  "cameraCuts": [
    { "frameStart": 0, "frameEnd": 60, "cameraBindingId": "3C7C8B2E-1234-5678-ABCD-000000000001" }
  ],
  "overwrite": false,
  "save": true
}
```

```jsonc
// Exemple : sequence.export (JSON)
{
  "sequencePath": "/Game/Cinematics/Seq/SEQ_Intro.SEQ_Intro",
  "format": "json",
  "include": {
    "tracks": ["Transform", "Visibility"],
    "keys": true
  },
  "frameRange": { "start": 0, "end": 120 },
  "worldActorPaths": true
}
```

```jsonc
// Exemple : sequence.export (CSV)
{
  "sequencePath": "/Game/Cinematics/Seq/SEQ_Intro.SEQ_Intro",
  "format": "csv",
  "include": {
    "tracks": ["Transform", "CameraCut"],
    "bindings": true
  },
  "flattenProperties": true
}
```

#### Éditeur

| Tool              | Type      | Description                               | Notes                                                |
|-------------------|-----------|-------------------------------------------|------------------------------------------------------|
| `level.select`    | read-only | Sélectionner des acteurs par filtres      | `mode=replace|add|remove`, support tags/classes        |
| `viewport.focus`  | read-only | Recentrer la caméra éditeur               | Acteurs, box ou location; `orbit` = cadrage orbital  |
| `camera.bookmark` | mixte     | `set/jump/list` de bookmarks caméra       | `persist=true` écrit dans le niveau (mutant + gate)  |

```jsonc
// Exemple : actor.spawn
{
  "classPath": "/Script/Engine.StaticMeshActor",
  "location": [0.0, 0.0, 150.0],
  "rotation": [0.0, 90.0, 0.0],
  "tags": ["SpawnedByMCP"],
  "select": true
}
```

```jsonc
// Exemple : level.select
{
  "filters": {
    "nameContains": "Dummy",
    "classNames": ["BP_TrainingDummy_C"],
    "tags": ["Training"]
  },
  "mode": "replace",
  "selectChildren": false
}
```

```jsonc
// Exemple : viewport.focus
{
  "actors": [
    "/Game/Maps/UEDPIE_0_Untitled.Untitled:PersistentLevel.BP_TrainingDummy_C_2",
    "/Game/Maps/UEDPIE_0_Untitled.Untitled:PersistentLevel.StaticMeshActor_15"
  ],
  "orbit": true,
  "transitionSec": 0.0
}
```

```jsonc
// Exemple : asset.rename
{
  "fromObjectPath": "/Game/Core/Old/BP_SpellProjectile.BP_SpellProjectile",
  "toPackagePath": "/Game/Core/Spells/BP_SpellProjectile"
}

// Exemple : asset.fix_redirectors
{
  "paths": ["/Game/Core", "/Game/Art"],
  "recursive": true
}

// Exemple : asset.batch_import
{
  "destPath": "/Game/Art/Characters/Orc",
  "files": ["D:/Imports/Orc/mesh/Orc.fbx", "D:/Imports/Orc/textures/Orc_Diffuse.png"],
  "preset": "fbx_character"
}
```

### Build & Test

| Tool               | Type     | Description                                     | Notes                                |
| ------------------ | -------- | ----------------------------------------------- | ------------------------------------ |
| `uat.buildcookrun` | external | Lance RunUAT BuildCookRun (cook/stage/package…) | Logs persistants, artefacts, dry-run |
| `automation.run_specs` | external | Lance Automation Tests via UnrealEditor-Cmd | Export log/XML, parsing basique |
| `gauntlet.run` | external | Lance Gauntlet sur build packagé | Logs UAT + Gauntlet, artifacts |

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
    "archive": true
  }
}
```

```json
{"tool":"automation.run_specs","params":{"engineRoot":"D:/UE_5.6","uproject":"D:/Proj/MyGame/MyGame.uproject","tests":["Project.Functional"],"timeoutMinutes":30}}
```

```json
{"tool":"gauntlet.run","params":{"engineRoot":"D:/UE_5.6","uproject":"D:/Proj/MyGame/MyGame.uproject","test":"MyGauntletSuite","platform":"Win64","build":{"path":"D:/Builds/MyGame/WindowsNoEditor"}}}
```

  > **Transactions & Undo** : chaque mutation est faite dans une transaction éditeur (Ctrl+Z possible).
  > **SCM** : si `RequireCheckout=true`, échec si l’asset n’est pas checkout.

## 🔐 Modèle de sécurité
- **Read-only par défaut** : `AllowWrite=false`.  
- **Dry-run** : si activé, les mutations renvoient un **plan** (`audit.actions[]`) sans rien changer.  
- **AllowedContentRoots** : seules les écritures dans ces chemins `/Game/...` sont permises.  
- **Allow/Deny-list** de tools : bloque/autorise par nom de tool, ex. `sc.submit`.

## 🧪 Tests & CI (aperçu)
- Protocol v1 testé (framing, ping/pong, timeouts).  
- Tools lecture testés avec l’Asset Registry.  
- Mutations enveloppées de transactions + SCM optionnel.  
- (Conseillé) Nightly Automation/Gauntlet pour projets complexes.

## 🛠 Développement local
- Côté UE : modules `Json`, `JsonUtilities`, `Sockets`, `Networking`, `DeveloperSettings`, `SourceControl`.
- Côté Python : lib standard (`socket`, `struct`, `json`, `argparse`, `selectors`).

## 🧯 Troubleshooting
- **Connection refused (127.0.0.1:12029)** : serveur Python non lancé / port erroné (voir Settings).  
- **WRITE_NOT_ALLOWED** : activer `AllowWrite` **et** vérifier `AllowedContentRoots`.  
- **SOURCE_CONTROL_REQUIRED** : activer/brancher le provider SCM ou désactiver `RequireCheckout`.  
- **Ping timeouts** : vérifier firewall/antivirus, `ReadTimeoutSec` et `HeartbeatIntervalSec`.

## 📜 Licence & Contribuer
- PRs bienvenues (features packs : Assets/Actors, Sequencer, Materials/MI, Niagara, Build/Test).  
- Merci d’ouvrir une issue avec un **use case** clair et critères d’acceptation.

