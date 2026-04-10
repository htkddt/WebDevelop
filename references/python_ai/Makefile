# python_ai — Python deps (pip) + engine shared library (NOT pip)
#
# From this directory:
#   make install-py-deps    # venv + requirements.txt + requirements-dev.txt
#   make fetch-engine-lib   # download release .tar.gz/.zip → lib/ (needs URL or GitHub token + tag)
#   make check-engine-lib   # fail if libm4engine missing under lib/ or libs/
#   make doctor             # install-py-deps + check-engine-lib + print find_lib()
#
# The .dylib/.so is supplied separately (git release, submodule, CI). See server/requirements.txt header.

PYTHON ?= python3
VENV     = server/.venv
PIP      = $(VENV)/bin/pip
PY       = $(VENV)/bin/python

LIB_NAME := $(shell $(PYTHON) -c "import sys; print('libm4engine.dylib' if sys.platform == 'darwin' else 'libm4engine.so')")

.PHONY: help venv install-py-deps fetch-engine-lib check-engine-lib doctor

help:
	@echo "python_ai Makefile"
	@echo "  make setup             — full setup: venv + pip deps + fetch latest engine lib + verify"
	@echo "  make install-py-deps   — create $(VENV) if needed, pip install server/requirements.txt + requirements-dev.txt"
	@echo "  make fetch-engine-lib  — fetch latest libm4engine from GitHub Release (ngoky/lib_c)"
	@echo "  make check-engine-lib  — lib/ or libs/ OR venv + m4engine-binary (pip git) via find_lib()"
	@echo "  make doctor            — install-py-deps + check-engine-lib + engine_ctypes.find_lib()"
	@echo ""
	@echo "From clean:  export M4_GITHUB_TOKEN=ghp_...  &&  make setup"

setup: install-py-deps fetch-engine-lib check-engine-lib
	@$(PY) -c "from engine_ctypes import find_lib; print('Ready:', find_lib())"

fetch-engine-lib:
	$(PYTHON) scripts/fetch_m4engine_lib.py

venv:
	@test -d $(VENV) || $(PYTHON) -m venv $(VENV)
	@$(PIP) install -q --upgrade pip

install-py-deps: venv
	$(PIP) install -r server/requirements.txt

check-engine-lib:
	@if test -f lib/$(LIB_NAME) || test -f libs/$(LIB_NAME); then \
		echo "OK: engine library present ($(LIB_NAME) under lib/ or libs/)"; \
	elif test -x $(PY) && (cd . && $(PY) -c "from engine_ctypes import find_lib; find_lib()"); then \
		echo "OK: engine library via pip (m4engine-binary)"; \
	else \
		echo "ERROR: missing $(LIB_NAME)"; \
		echo "  pip install \"m4engine-binary @ git+https://...\" (see server/requirements.txt header), or"; \
		echo "  copy the built library to lib/ or libs/."; \
		exit 1; \
	fi

doctor: install-py-deps check-engine-lib
	@$(PY) -c "from engine_ctypes import find_lib; print('find_lib():', find_lib())"
