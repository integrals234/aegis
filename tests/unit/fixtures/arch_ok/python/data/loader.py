"""Fixture module: data layer depending on the substrate, which is permitted."""

from common.config import load


def rows() -> list[dict]:
    return [load()]
