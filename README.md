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

