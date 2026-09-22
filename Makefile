SHELL := /bin/bash
VERSION=$(shell cat VERSION)

ifeq ($(shell uname -s),Darwin)
LOADABLE_EXTENSION=dylib
else
LOADABLE_EXTENSION=so
endif

ifdef IS_MACOS_ARM
RENAME_WHEELS_ARGS=--is-macos-arm
else
RENAME_WHEELS_ARGS=
endif

ifdef python
PYTHON=$(python)
else
PYTHON=python3
endif

PREFIX=dist
BUILD=build
BUILD_RELEASE=build_release
CMAKE_FLAGS?=
SOURCES=CMakeLists.txt VERSION engine/CMakeLists.txt engine/laya_engine.hpp \
	sqlite/CMakeLists.txt sqlite/src/sqlaya.cpp sqlite/src/sqlaya.h.in

# SQLite loadable module
TARGET_LOADABLE_FILE=$(PREFIX)/debug/laya.$(LOADABLE_EXTENSION)
TARGET_LOADABLE=$(TARGET_LOADABLE_FILE)
TARGET_LOADABLE_RELEASE_FILE=$(PREFIX)/release/laya.$(LOADABLE_EXTENSION)
TARGET_LOADABLE_RELEASE=$(TARGET_LOADABLE_RELEASE_FILE)

# SQLite static library
TARGET_STATIC_FILE=$(PREFIX)/debug/libsqlite_laya.a
TARGET_STATIC_H=$(PREFIX)/debug/sqlaya.h
TARGET_STATIC=$(TARGET_STATIC_FILE) $(TARGET_STATIC_H)
TARGET_STATIC_RELEASE_FILE=$(PREFIX)/release/libsqlite_laya.a
TARGET_STATIC_RELEASE_H=$(PREFIX)/release/sqlaya.h
TARGET_STATIC_RELEASE=$(TARGET_STATIC_RELEASE_FILE) $(TARGET_STATIC_RELEASE_H)

# Python package
PYTHON_PACKAGE=sqlite/bindings/python
INTERMEDIATE_PYPACKAGE_EXTENSION=$(PYTHON_PACKAGE)/sqlite_laya/
TARGET_WHEELS=$(PREFIX)/debug/wheels
TARGET_WHEELS_RELEASE=$(PREFIX)/release/wheels

# PostgreSQL extension (postgres/), built in the same tree
PG_CMAKE_FLAGS=-DSQLAYA_POSTGRES=ON $(if $(LAYA_MODEL_DIR),-DLAYA_MODEL_DIR=$(abspath $(LAYA_MODEL_DIR))) $(if $(LAYA_OPTIONS),'-DLAYA_OPTIONS=$(LAYA_OPTIONS)')

# Model store
MODEL_DIR=models/laya
MODEL_VARIANT?=english

$(PREFIX):
	mkdir -p $(PREFIX)/debug
	mkdir -p $(PREFIX)/release

$(TARGET_LOADABLE): $(PREFIX) $(SOURCES)
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS) && cmake --build $(BUILD) --parallel --target sqlaya
	cp $(BUILD)/sqlite/laya.$(LOADABLE_EXTENSION) $(TARGET_LOADABLE_FILE)

$(TARGET_LOADABLE_RELEASE): $(PREFIX) $(SOURCES)
	cmake -DCMAKE_BUILD_TYPE=Release -S . -B $(BUILD_RELEASE) $(CMAKE_FLAGS) && cmake --build $(BUILD_RELEASE) --parallel --target sqlaya
	cp $(BUILD_RELEASE)/sqlite/laya.$(LOADABLE_EXTENSION) $(TARGET_LOADABLE_RELEASE_FILE)

$(TARGET_STATIC): $(PREFIX) $(SOURCES)
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS) && cmake --build $(BUILD) --parallel --target sqlaya-static
	cp $(BUILD)/sqlite/libsqlite_laya.a $(TARGET_STATIC_FILE)
	cp $(BUILD)/sqlite/sqlaya.h $(TARGET_STATIC_H)

$(TARGET_STATIC_RELEASE): $(PREFIX) $(SOURCES)
	cmake -DCMAKE_BUILD_TYPE=Release -S . -B $(BUILD_RELEASE) $(CMAKE_FLAGS) && cmake --build $(BUILD_RELEASE) --parallel --target sqlaya-static
	cp $(BUILD_RELEASE)/sqlite/libsqlite_laya.a $(TARGET_STATIC_RELEASE_FILE)
	cp $(BUILD_RELEASE)/sqlite/sqlaya.h $(TARGET_STATIC_RELEASE_H)

$(TARGET_WHEELS): $(PREFIX)
	mkdir -p $(TARGET_WHEELS)

$(TARGET_WHEELS_RELEASE): $(PREFIX)
	mkdir -p $(TARGET_WHEELS_RELEASE)

loadable: $(TARGET_LOADABLE)
loadable-release: $(TARGET_LOADABLE_RELEASE)

static: $(TARGET_STATIC)
static-release: $(TARGET_STATIC_RELEASE)

# Builds the laya command line tool from the submodule; used by the parity test.
cli: loadable
	cmake --build $(BUILD) --parallel --target laya-cli

clean:
	rm -rf dist/*

python: $(TARGET_WHEELS) $(TARGET_LOADABLE) $(PYTHON_PACKAGE)/setup.py $(PYTHON_PACKAGE)/sqlite_laya/__init__.py sqlite/scripts/rename-wheels.py
	cp $(TARGET_LOADABLE_FILE) $(INTERMEDIATE_PYPACKAGE_EXTENSION)
	rm $(TARGET_WHEELS)/sqlite_laya* || true
	$(PYTHON) -m pip wheel $(PYTHON_PACKAGE)/ -w $(TARGET_WHEELS)
	$(PYTHON) sqlite/scripts/rename-wheels.py $(TARGET_WHEELS) $(RENAME_WHEELS_ARGS)
	echo "✅ generated python wheel"

python-release: $(TARGET_WHEELS_RELEASE) $(TARGET_LOADABLE_RELEASE) $(PYTHON_PACKAGE)/setup.py $(PYTHON_PACKAGE)/sqlite_laya/__init__.py sqlite/scripts/rename-wheels.py
	cp $(TARGET_LOADABLE_RELEASE_FILE) $(INTERMEDIATE_PYPACKAGE_EXTENSION)
	rm $(TARGET_WHEELS_RELEASE)/sqlite_laya* || true
	$(PYTHON) -m pip wheel $(PYTHON_PACKAGE)/ -w $(TARGET_WHEELS_RELEASE)
	$(PYTHON) sqlite/scripts/rename-wheels.py $(TARGET_WHEELS_RELEASE) $(RENAME_WHEELS_ARGS)
	echo "✅ generated release python wheel"

python-versions: $(PYTHON_PACKAGE)/version.py.tmpl
	VERSION=$(VERSION) envsubst < $(PYTHON_PACKAGE)/version.py.tmpl > $(PYTHON_PACKAGE)/sqlite_laya/version.py
	echo "✅ generated $(PYTHON_PACKAGE)/sqlite_laya/version.py"

# Downloads a pinned Laya checkpoint (requires `pip install huggingface_hub`).
model:
	$(PYTHON) laya.cpp/scripts/download_model.py --variant $(MODEL_VARIANT)
	mkdir -p $(dir $(MODEL_DIR))
	rm -rf $(MODEL_DIR)
	mv laya.cpp/models/laya $(MODEL_DIR)
	echo "✅ downloaded $(MODEL_VARIANT) checkpoint to $(MODEL_DIR)"

postgres:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS) $(PG_CMAKE_FLAGS) && cmake --build $(BUILD) --parallel --target pglaya

# Installs into the PostgreSQL directories reported by pg_config (may need sudo).
postgres-install: postgres
	cmake --install $(BUILD)

# Runs pg_regress on a temporary instance; the extension must be installed.
test-postgres:
	ctest --test-dir $(BUILD) --output-on-failure

test-loadable:
	$(PYTHON) sqlite/tests/test-loadable.py

test-python:
	$(PYTHON) sqlite/tests/test-python.py

test:
	make test-loadable
	make test-python

.PHONY: clean test \
	loadable loadable-release static static-release cli \
	python python-release python-versions model \
	postgres postgres-install test-postgres test-loadable test-python
