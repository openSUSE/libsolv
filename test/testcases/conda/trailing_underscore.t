# a trailing '_' (or '-') is kept as part of the last version segment,
# so openssl-like versions sort right: 1.0.1dev < 1.0.1_ < 1.0.1a < 1.0.1
repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: pkg 1.0_ - noarch
#>=Flv: 0
#>=Prv: pkg = 1.0_
#>=Pkg: pkg 1.0 - noarch
#>=Flv: 0
#>=Prv: pkg = 1.0
#>=Pkg: tmux 3.7_ - noarch
#>=Flv: 0
#>=Prv: tmux = 3.7_
#>=Pkg: tmux 3.7c_ - noarch
#>=Flv: 0
#>=Prv: tmux = 3.7c_
system unset conda system

evrcmp 1.0.1dev 1.0.1_
evrcmp 1.0.1_ 1.0.1a
evrcmp 1.0.1a 1.0.1
evrcmp 1.0_ 1.0
evrcmp 1.0_ 1.0rc1
evrcmp 1.0a_ 1.0a
evrcmp 3.7_ 3.7c_
evrcmp 1.0- 1.0_
evrcmp 1.0_ 1.0.0
evrcmp 1._ 1
evrcmp 1.0_+a 1.0+a
evrcmp 1.0_1 1.0.1
result jobs <inline>
#>job noop provides 1.0.1dev < 1.0.1_
#>job noop provides 1.0.1_ < 1.0.1a
#>job noop provides 1.0.1a < 1.0.1
#>job noop provides 1.0_ < 1.0
#>job noop provides 1.0_ < 1.0rc1
#>job noop provides 1.0a_ > 1.0a
#>job noop provides 3.7_ < 3.7c_
#>job noop provides 1.0- = 1.0_
#>job noop provides 1.0_ < 1.0.0
#>job noop provides 1._ < 1
#>job noop provides 1.0_+a < 1.0+a
#>job noop provides 1.0_1 = 1.0.1

nextjob
job install provides pkg <CONDA> <1.0rc1
result transaction,problems <inline>
#>install pkg-1.0_-0.noarch@available

nextjob
job install provides tmux <CONDA> 3.7.*
result transaction,problems <inline>
#>install tmux-3.7c_-0.noarch@available
