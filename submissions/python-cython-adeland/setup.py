import os
from setuptools import Extension, setup
from Cython.Build import cythonize

sanitize = os.environ.get('SANITIZE') == '1'
flags = ['-O3', '-fno-strict-aliasing', '-Wall']
links = []
if sanitize:
    flags += ['-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    links += ['-fsanitize=address,undefined']
ext = Extension('engine', ['engine.pyx', '_deps/llhttp/src/api.c', '_deps/llhttp/src/http.c',
                           '_deps/llhttp/src/llhttp.c', '_deps/yyjson/src/yyjson.c'],
                include_dirs=['.', '_deps/llhttp/include', '_deps/yyjson/src'],
                depends=['native.h', 'native/memory.h'],
                libraries=['sqlite3', 'crypto'], extra_compile_args=flags, extra_link_args=links)
setup(name='twelve-cython', version='0.1.0', ext_modules=cythonize([ext],
      compiler_directives={'language_level': 3, 'boundscheck': False, 'wraparound': False,
                           'initializedcheck': False, 'cdivision': True}))
