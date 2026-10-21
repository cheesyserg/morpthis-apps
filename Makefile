MORPTHIS_SDK ?= $(abspath ../morpthis-sdk)
export MORPTHIS_SDK

APPS := $(notdir $(wildcard apps/*))

.PHONY: all validate host test device check-abi package clean $(APPS)

all: host

validate:
	python3 tools/validate-manifest.py --all apps

host: validate
	@set -e; for app in $(APPS); do \
		echo "==> $$app (host)"; \
		$(MAKE) -C apps/$$app MORPTHIS_MODE=host; \
	done

test: host
	@set -e; for app in $(APPS); do \
		echo "==> $$app (test)"; \
		$(MAKE) -C apps/$$app MORPTHIS_MODE=host test; \
	done

device: validate
	@set -e; for app in $(APPS); do \
		echo "==> $$app (device)"; \
		$(MAKE) -C apps/$$app MORPTHIS_MODE=device; \
	done

check-abi: device
	@set -e; for app in $(APPS); do \
		$(MAKE) -C apps/$$app MORPTHIS_MODE=device check-abi; \
	done

package: validate
	@mkdir -p dist
	@set -e; for app in $(APPS); do \
		./tools/package.sh apps/$$app dist; \
	done

clean:
	@set -e; for app in $(APPS); do \
		$(MAKE) -C apps/$$app clean; \
	done
	rm -rf dist

$(APPS):
	$(MAKE) -C apps/$@
