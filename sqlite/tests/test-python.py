import unittest
import sqlite3
import sqlite_laya

class TestSqliteLayaPython(unittest.TestCase):
  def test_path(self):
    self.assertEqual(type(sqlite_laya.loadable_path()), str)
  
  def test_load(self):
    db = sqlite3.connect(':memory:')
    db.enable_load_extension(True)
    sqlite_laya.load(db)

    version, = db.execute('select laya_version()').fetchone()
    self.assertEqual(version[0], "v")
    
if __name__ == '__main__':
    unittest.main()