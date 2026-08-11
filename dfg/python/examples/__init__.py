"""Runnable examples, and the node library they are built from.

Everything here is synthetic and deterministic: no codecs, no downloads, no files
written, no plotting. Each script exposes ``build_registry()``,
``build_blueprint()``, and ``main()`` so a test can drive it in process rather than
scraping its output.

**This is the only place in the port that may import numpy or pyarrow.** The core
under ``dfg/`` is stdlib-only, and ``tests/test_core_is_stdlib_only.py`` enforces
that by walking its imports.
"""
