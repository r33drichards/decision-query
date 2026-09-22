from setuptools import setup, Extension
import os
import platform

version = {}
with open("sqlite_laya/version.py") as fp:
    exec(fp.read(), version)

VERSION = version['__version__']

system = platform.system()
machine = platform.machine()

print(system, machine)

if system == 'Darwin':
  if machine not in ['x86_64', 'arm64']:
    raise Exception("unsupported platform")  
elif system == 'Linux':
  if machine not in ['x86_64']:
    raise Exception("unsupported platform")
elif system == 'Windows':
  if machine not in ['AMD64']:
    raise Exception("unsupported platform")
else: 
  raise Exception("unsupported platform")

setup(
    name="sqlite-laya",
    description="SQLite extension for querying data with Laya typed decisions",
    long_description="SQLite extension for querying data with Laya typed decisions",
    long_description_content_type="text/markdown",
    author="r33drichards",
    url="https://github.com/r33drichards/sqlaya",
    license="MIT License",
    version=VERSION,
    packages=["sqlite_laya"],
    package_data={"sqlite_laya": ['*.so', '*.dylib', '*.dll']},
    install_requires=[],
    # Adding an Extension makes `pip wheel` believe that this isn't a 
    # pure-python package. The noop.c was added since the windows build
    # didn't seem to respect optional=True
    ext_modules=[Extension("noop", ["noop.c"], optional=True)],
    extras_require={"test": ["pytest"]},
    python_requires=">=3.7",
)