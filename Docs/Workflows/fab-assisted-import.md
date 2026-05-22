# Fab-Assisted Import

This workflow keeps Fab acquisition interactive while making the surrounding
asset evidence repeatable.

UEMCP does not automate Fab login, entitlement, purchase, license acceptance, or payment. Stop at those visible editor UI prompts and let the user make the account or licensing decision.

## Protocol

1. Start the interactive asset workflow against the consuming project.
2. Pass expected content roots with `-ExpectedAssetRoots` when known.
3. Wait for editor-backed readiness.
4. Run `asset_intake_snapshot` for the target roots.
5. Use the visible Fab/editor UI to acquire or import assets.
6. Stop for any account, entitlement, purchase, license, or payment prompt.
7. Run a second `asset_intake_snapshot` for the same roots.
8. Run `asset_intake_diff`.
9. Write a project-owned manifest with `asset_intake_write_manifest`.
10. Run project-pack gates before placement or Blueprint wiring.

## Manifest Checklist

Record these notes in the manifest:

- source URL.
- Asset or pack name.
- Publisher name when visible.
- License or entitlement note confirmed by the user.
- Import target roots.
- Any files or folders intentionally ignored after import.

## Allowed Agent Actions

- Navigate visible editor UI when the user permits it.
- Capture before/after UEMCP evidence.
- Organize, inspect, and validate assets with editor-backed tools.
- Create project-owned manifests under `Tools/UEMCP/asset-intake`.

## Stop Points

Stop and ask when:

- Fab asks for account access or credentials.
- Fab asks for a transaction or entitlement decision.
- Fab asks for a license or usage confirmation.
- Asset provenance is not clear enough to record in the manifest.
