repo available 0 testtags <inline>
#>=Pkg: bash 1 1 noarch
#>=Fls: /usr/bin/bash
#>=Fls: /usr/bin/bashbug
#>=Pkg: bash 2 1 noarch
#>=Fls: /usr/bin/bash
#>=Fls: /usr/bin/bashbug
#>=Pkg: coreutils 1 1 noarch
#>=Fls: /usr/bin/basename
system i686 rpm

job noop selection /usr/bin/ba* filelist,glob
result jobs <inline>
#>job noop oneof bash-1-1.noarch@available bash-2-1.noarch@available
#>job noop pkg coreutils-1-1.noarch@available [noautoset]

nextjob
job noop selection /usr/bin/ba* filelist,glob,flat
result jobs <inline>
#>job noop oneof bash-1-1.noarch@available bash-2-1.noarch@available coreutils-1-1.noarch@available
