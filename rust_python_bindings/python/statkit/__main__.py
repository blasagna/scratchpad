"""Entry point for ``python -m statkit``.

The installed ``statkit-py`` console script (see ``pyproject.toml``) calls the
same :func:`statkit.cli.main`.
"""

import sys

from .cli import main

sys.exit(main())
