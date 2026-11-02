import os
from app import app

if __name__ == "__main__":
    host = os.environ.get("HOST", "127.0.0.1")
    port = int(os.environ.get("PORT", "3000"))
    workers = int(os.environ.get("WORKERS", "1"))
    app.run(host=host, port=port, workers=workers)
