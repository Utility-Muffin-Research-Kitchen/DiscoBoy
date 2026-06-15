# Disco Boy - Leaf music player.
#
# Leaf stages this app with `make package-platform PLATFORM=mlp1`, which builds
# the aarch64 binary in the mlp1-toolchain container and assembles the staged pak
# under build/<platform>/package/DiscoBoy.pak (Leaf then deploys that dir).

MLP1_PACKAGE := build/mlp1/package/DiscoBoy.pak
MLP1_BIN     := ports/mlp1/pak/bin/discoboy

.PHONY: package-platform package-mlp1 mlp1 clean

package-platform:
	@test -n "$(PLATFORM)" || { echo "usage: make package-platform PLATFORM=<platform>" >&2; exit 1; }
	@case "$(PLATFORM)" in \
		mlp1) $(MAKE) package-mlp1 ;; \
		*) echo "unsupported Disco Boy package platform: $(PLATFORM)" >&2; exit 1 ;; \
	esac

# Cross-compile the aarch64 binary (Docker mlp1-toolchain).
mlp1:
	@./scripts/build-mlp1.sh

# Build, then assemble the staged pak: pak/ template + the built binary.
package-mlp1: mlp1
	@rm -rf "$(MLP1_PACKAGE)"
	@mkdir -p "$(MLP1_PACKAGE)/bin"
	@cp -R pak/launch.sh pak/pak.json pak/res "$(MLP1_PACKAGE)/"
	@cp "$(MLP1_BIN)" "$(MLP1_PACKAGE)/bin/discoboy"
	@echo "=== Packaged: $(MLP1_PACKAGE) ==="

clean:
	rm -rf build ports/*/pak/bin
