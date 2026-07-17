# Disco Boy - Leaf music player.
#
# Leaf stages this app with `make package-platform PLATFORM=mlp1`, which builds
# the aarch64 binary in the mlp1-toolchain container and assembles the staged pak
# under build/<platform>/package/DiscoBoy.pak (Leaf then deploys that dir).

MLP1_PACKAGE := build/mlp1/package/DiscoBoy.pak
MLP1_ARCHIVE := build/mlp1/DiscoBoy.pak.zip
MLP1_BIN     := ports/mlp1/pak/bin/discoboy
PAK_VERSION  := $(shell python3 -c 'import json; print(json.load(open("pak/pak.json"))["pak_version"])')

.PHONY: package-platform package-mlp1 package-archive package-smoke mlp1 test test-sources pakrat-metadata-check clean

test: test-sources pakrat-metadata-check

test-sources:
	$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
		-Icmd/discoboy cmd/discoboy/disco_sources.c cmd/discoboy/disco_sources_test.c \
		-o /tmp/discoboy-sources-test
	/tmp/discoboy-sources-test
	rm -f /tmp/discoboy-sources-test

pakrat-metadata-check:
	@python3 scripts/pakrat-metadata-check.py

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

package-archive: package-mlp1
	@python3 scripts/package-mlp1.py \
		--package "$(MLP1_PACKAGE)" \
		--archive "$(MLP1_ARCHIVE)"

package-smoke: package-archive
	@python3 scripts/package-smoke.py \
		--package "$(MLP1_PACKAGE)" \
		--archive "$(MLP1_ARCHIVE)" \
		--version "$(PAK_VERSION)"

clean:
	rm -rf build ports/*/pak/bin
