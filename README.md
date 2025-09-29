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
- **Actors v1 (Editor)** : `actor.spawn / actor.destroy / actor.attach / actor.transform / actor.tag` (transactions, sélection, audit).
- **Camera helpers (Editor)** : `level.select / viewport.focus / camera.bookmark` (navigation + bookmarks, session & persistance).
- **Settings Plugin** : Project Settings → **Plugins → Unreal MCP** (Network, Security, SCM, Logging, Diagnostics).

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

