# MCP CLI

CLI Typer permettant de piloter le serveur Unreal MCP depuis la ligne de commande.

## Installation

```bash
cd Python/cli
pip install -e .
```

La commande `mcp` est alors disponible (`mcp --help`). Le serveur cible peut être indiqué via `--server` ou la variable d’environnement `MCP_SERVER` (`127.0.0.1:8765` par défaut).

## Commandes principales

| Commande              | Description                                                 |
|-----------------------|-------------------------------------------------------------|
| `mcp run`             | Exécute un tool MCP unique avec paramètres JSON/YAML        |
| `mcp recipe run`      | Exécute une recette YAML (steps, dépendances, parallelisme) |
| `mcp recipe test`     | Valide la recette, affiche le plan, dry-run optionnel       |

Options transverses : `--dry-run`, `--retry`, `--parallel`, `--timeout`, `--vars`/`--vars-file`, `--select` (JMESPath), `--output json|yaml`, `--log-level`, `--env KEY=VALUE`.

## Exemples rapides

```bash
# Tool simple
mcp run level.save_open --params-json '{"modifiedOnly":true}'

# Pipeline YAML avec variables
mcp recipe run ./pipelines/content_cleanup.yaml --vars GAME_ROOT=/Game/Core

# Test d'une recette (plan + audits dry-run)
mcp recipe test ./pipelines/content_cleanup.yaml --dry-run --output yaml
```

## Format de recette

```yaml
version: 1
vars:
  GAME_ROOT: "/Game/Core"
steps:
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

Les variables `${VAR}` proviennent du fichier, de `--vars-file`, de `--vars` et de l’environnement. Les expressions `${{ steps.* }}` accèdent aux sorties des steps précédents. Les audits (`actions`/`diffs`) sont fusionnés dans le récapitulatif final.
