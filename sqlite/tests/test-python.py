import unittest
import sqlite3
import decision_query

class TestSqliteLayaPython(unittest.TestCase):
  def test_path(self):
    self.assertEqual(type(decision_query.loadable_path()), str)
  
  def test_load(self):
    db = sqlite3.connect(':memory:')
    db.enable_load_extension(True)
    decision_query.load(db)

    version, = db.execute('select dq_version()').fetchone()
    self.assertEqual(version[0], "v")
    
if __name__ == '__main__':
    unittest.main()