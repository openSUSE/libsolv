# repository priority prunes candidates before the lib*-name
# preference: the higher-priority non-lib provider must win
repo system 0 empty
repo hikrepo 1 testtags <inline>
#>=Pkg: hik-bin 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
repo librepo 0 testtags <inline>
#>=Pkg: X 1 1 x86_64
#>=Req: libfake.so.1()(64bit)
#>=Pkg: lib64fake1 1 1 x86_64
#>=Prv: libfake.so.1()(64bit)
system x86_64 rpm system
job install name X
result transaction,problems <inline>
#>install X-1-1.x86_64@librepo
#>install hik-bin-1-1.x86_64@hikrepo
