"""项目根和显式复用的本地工具/依赖缓存; 产物始终由各入口放入项目 build."""

import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = Path(os.environ.get("ASTRA_CACHE_ROOT", ROOT / "build")).expanduser().resolve()
