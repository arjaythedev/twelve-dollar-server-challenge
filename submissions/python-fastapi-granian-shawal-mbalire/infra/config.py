"""Settings, read once from the environment (12-factor). Fails fast on anything missing."""

from collections.abc import Mapping
from dataclasses import dataclass


@dataclass(slots=True, frozen=True)
class Settings:
    sqlite_path: str
    jwt_secret: bytes
    profile_out: str | None = None  # PROFILE_OUT: write a cProfile dump here on shutdown


def load_settings(env: Mapping[str, str]) -> Settings:
    missing = [name for name in ("SQLITE_PATH", "JWT_SECRET") if not env.get(name)]
    if missing:
        raise SystemExit(f"missing environment variables: {', '.join(missing)}")
    return Settings(
        sqlite_path=env["SQLITE_PATH"],
        jwt_secret=env["JWT_SECRET"].encode(),
        profile_out=env.get("PROFILE_OUT") or None,
    )
