"""Violation 7: the substrate importing the data layer inverts the DAG."""

from data.loader import rows


def load() -> list:
    return rows()
