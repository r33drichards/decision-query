import os
import sqlite3

from sqlite_laya.version import __version_info__, __version__


def loadable_path():
  loadable_path = os.path.join(os.path.dirname(__file__), "laya")
  return os.path.normpath(loadable_path)


def load(connection: sqlite3.Connection) -> None:
  connection.load_extension(loadable_path())
