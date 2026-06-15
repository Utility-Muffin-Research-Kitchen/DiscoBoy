#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DISCOBOY_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE="$(cd "$DISCOBOY_DIR/.." && pwd)"   # siblings: DiscoBoy, Catastrophe, Jawaka
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
echo "=== Building Disco Boy for MLP1 (workspace: $WORKSPACE) ==="
docker run --rm -v "$WORKSPACE":/workspace -w /workspace/DiscoBoy "$IMAGE" make -C ports/mlp1
echo "=== Build complete: ports/mlp1/pak/bin/discoboy ==="
