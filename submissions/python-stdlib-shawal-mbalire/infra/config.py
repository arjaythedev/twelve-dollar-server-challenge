"""Settings, read once from the environment (12-factor). Fails fast on anything missing."""

from collections.abc import Mapping
from dataclasses import dataclass

REQUIRED = ("SQLITE_PATH", "JWT_SECRET", "HOST", "PORT")


@dataclass(slots=True, frozen=True)
class Settings:
    sqlite_path: str
    jwt_secret: bytes
    host: str
    port: int
    profile_out: str | None = None  # PROFILE_OUT: write a cProfile dump here on shutdown
    profile_hops_out: str | None = None  # PROFILE_HOPS_OUT: time every request hop, write JSON here


def load_settings(environment: Mapping[str, str]) -> Settings:
    if missing := [name for name in REQUIRED if not environment.get(name)]:
        raise SystemExit(f"missing environment variables: {', '.join(missing)}")
    return Settings(
        sqlite_path=environment["SQLITE_PATH"],
        jwt_secret=environment["JWT_SECRET"].encode(),
        host=environment["HOST"],
        port=int(environment["PORT"]),
        profile_out=environment.get("PROFILE_OUT") or None,
        profile_hops_out=environment.get("PROFILE_HOPS_OUT") or None,
    )
