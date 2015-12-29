from distutils.core import setup, Extension

ridmodule = Extension('rid',
                    include_dirs = ['../../include',],
                    extra_link_args = ['-L../bin', '-lrid', '-Wl,-rpath-link=../bin'],  
                    sources = ['ridmodule.c'])

setup(name = 'PackageName',
       version = '1.0',
       description = 'RID library for Python',
       author = 'Antonio Rodrigues',
       author_email = 'adamiaonr@cmu.edu',
       ext_modules = [ridmodule])
