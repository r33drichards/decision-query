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
CMAKE_FLAGS?=
SOURCES=src/sqlaya.cpp src/sqlaya.h.in CMakeLists.txt VERSION

# Loadable Output
TARGET_LOADABLE_FILE=$(PREFIX)/debug/laya.$(LOADABLE_EXTENSION)
TARGET_LOADABLE=$(TARGET_LOADABLE_FILE)
TARGET_LOADABLE_RELEASE_FILE=$(PREFIX)/release/laya.$(LOADABLE_EXTENSION)
TARGET_LOADABLE_RELEASE=$(TARGET_LOADABLE_RELEASE_FILE)

# Static Output
TARGET_STATIC_FILE=$(PREFIX)/debug/libsqlite_laya.a
TARGET_STATIC_H=$(PREFIX)/debug/sqlaya.h
TARGET_STATIC=$(TARGET_STATIC_FILE) $(TARGET_STATIC_H)
TARGET_STATIC_RELEASE_FILE=$(PREFIX)/release/libsqlite_laya.a
TARGET_STATIC_RELEASE_H=$(PREFIX)/release/sqlaya.h
TARGET_STATIC_RELEASE=$(TARGET_STATIC_RELEASE_FILE) $(TARGET_STATIC_RELEASE_H)

# Python Output
INTERMEDIATE_PYPACKAGE_EXTENSION=bindings/python/sqlite_laya/
TARGET_WHEELS=$(PREFIX)/debug/wheels
TARGET_WHEELS_RELEASE=$(PREFIX)/release/wheels

# Model store
MODEL_DIR=models/laya
MODEL_VARIANT?=english

$(PREFIX):
	mkdir -p $(PREFIX)/debug
	mkdir -p $(PREFIX)/release

$(TARGET_LOADABLE): $(PREFIX) $(SOURCES)
	cmake -S . -B build $(CMAKE_FLAGS) && cmake --build build --parallel
	cp build/laya.$(LOADABLE_EXTENSION) $(TARGET_LOADABLE_FILE)

$(TARGET_LOADABLE_RELEASE): $(PREFIX) $(SOURCES)
	cmake -DCMAKE_BUILD_TYPE=Release -S . -B build_release $(CMAKE_FLAGS) && cmake --build build_release --parallel
	cp build_release/laya.$(LOADABLE_EXTENSION) $(TARGET_LOADABLE_RELEASE_FILE)

$(TARGET_STATIC): $(PREFIX) $(SOURCES)
	cmake -S . -B build $(CMAKE_FLAGS) && cmake --build build --parallel
	cp build/libsqlite_laya.a $(TARGET_STATIC_FILE)
	cp build/sqlaya.h $(TARGET_STATIC_H)

$(TARGET_STATIC_RELEASE): $(PREFIX) $(SOURCES)
	cmake -DCMAKE_BUILD_TYPE=Release -S . -B build_release $(CMAKE_FLAGS) && cmake --build build_release --parallel
	cp build_release/libsqlite_laya.a $(TARGET_STATIC_RELEASE_FILE)
	cp build_release/sqlaya.h $(TARGET_STATIC_RELEASE_H)

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
	cmake --build build --parallel --target laya-cli

clean:
	rm -rf dist/*

python: $(TARGET_WHEELS) $(TARGET_LOADABLE) bindings/python/setup.py bindings/python/sqlite_laya/__init__.py scripts/rename-wheels.py
	cp $(TARGET_LOADABLE_FILE) $(INTERMEDIATE_PYPACKAGE_EXTENSION)
	rm $(TARGET_WHEELS)/sqlite_laya* || true
	$(PYTHON) -m pip wheel bindings/python/ -w $(TARGET_WHEELS)
	$(PYTHON) scripts/rename-wheels.py $(TARGET_WHEELS) $(RENAME_WHEELS_ARGS)
	echo "✅ generated python wheel"

python-release: $(TARGET_WHEELS_RELEASE) $(TARGET_LOADABLE_RELEASE) bindings/python/setup.py bindings/python/sqlite_laya/__init__.py scripts/rename-wheels.py
	cp $(TARGET_LOADABLE_RELEASE_FILE) $(INTERMEDIATE_PYPACKAGE_EXTENSION)
	rm $(TARGET_WHEELS_RELEASE)/sqlite_laya* || true
	$(PYTHON) -m pip wheel bindings/python/ -w $(TARGET_WHEELS_RELEASE)
	$(PYTHON) scripts/rename-wheels.py $(TARGET_WHEELS_RELEASE) $(RENAME_WHEELS_ARGS)
	echo "✅ generated release python wheel"

python-versions: bindings/python/version.py.tmpl
	VERSION=$(VERSION) envsubst < bindings/python/version.py.tmpl > bindings/python/sqlite_laya/version.py
	echo "✅ generated bindings/python/sqlite_laya/version.py"

# Downloads a pinned Laya checkpoint (requires `pip install huggingface_hub`).
model:
	$(PYTHON) vendor/laya.cpp/scripts/download_model.py --variant $(MODEL_VARIANT)
	mkdir -p $(dir $(MODEL_DIR))
	rm -rf $(MODEL_DIR)
	mv vendor/laya.cpp/models/laya $(MODEL_DIR)
	echo "✅ downloaded $(MODEL_VARIANT) checkpoint to $(MODEL_DIR)"

test-loadable:
	$(PYTHON) tests/test-loadable.py

test-python:
	$(PYTHON) tests/test-python.py

test:
	make test-loadable
	make test-python

.PHONY: clean test \
	loadable loadable-release static static-release cli \
	python python-release python-versions model \
	test-loadable test-python
