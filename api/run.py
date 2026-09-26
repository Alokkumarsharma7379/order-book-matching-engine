"""Container entrypoint: migrate first, then start exactly one engine worker."""
from alembic import command
from alembic.config import Config
import uvicorn


def main():
    command.upgrade(Config("alembic.ini"), "head")
    uvicorn.run("api.main:app", host="0.0.0.0", port=8000, workers=1)


if __name__ == "__main__":
    main()
