repo available 0 testtags <inline>
#>=Pkg: a 1 1 noarch
#>=Fls: /usr/bin/a
#>=Fls: /usr/lib/python3/os.py
#>=Fls: /usr/lib/python3/site.py
#>=Fls: /usr/lib64/libfoo.so
#>=Pkg: b 1 1 noarch
#>=Fls: /usr/lib/python3/sub/mod.py
#>=Fls: /usr/lib64/libbar.so
#>=Fls: /usr/share/doc/b/README
#>=Pkg: c 1 1 noarch
#>=Fls: /Usr/Lib64/CaseFile
#>=Fls: /usr/lib/python3
#>=Fls: /usr/lib64x/libbaz.so
#>=Fls: /usr/share
repo second 0 testtags <inline>
#>=Pkg: d 1 1 noarch
#>=Fls: /usr/lib64/libd.so
#>=Fls: /opt/d/bin/d
system i686 rpm

# glob with a literal directory prefix, only directories below it match
# (d is in a second repo, where /usr/lib64 gets the directory id that /usr/bin has in the first)
job noop selection /usr/lib64/* filelist,glob,flat
result jobs <inline>
#>job noop oneof a-1-1.noarch@available b-1-1.noarch@available d-1-1.noarch@second

nextjob
job noop selection /usr/lib/python3/* filelist,glob,flat
result jobs <inline>
#>job noop oneof a-1-1.noarch@available b-1-1.noarch@available

# wildcards in the last component and in the middle of the path
nextjob
job noop selection /usr/lib/python3/s?* filelist,glob,flat
result jobs <inline>
#>job noop oneof a-1-1.noarch@available b-1-1.noarch@available

nextjob
job noop selection /usr/lib/*/os.py filelist,glob,flat
result jobs <inline>
#>job noop pkg a-1-1.noarch@available [noautoset]

nextjob
job noop selection /usr/*/doc/* filelist,glob,flat
result jobs <inline>
#>job noop pkg b-1-1.noarch@available [noautoset]

# a directory that is not below the prefix must not match (/usr/lib64x vs /usr/lib64/)
nextjob
job noop selection /usr/lib64x/* filelist,glob,flat
result jobs <inline>
#>job noop pkg c-1-1.noarch@available [noautoset]

# bracket expression ends the literal prefix
nextjob
job noop selection /usr/[l]ib64/* filelist,glob,flat
result jobs <inline>
#>job noop oneof a-1-1.noarch@available b-1-1.noarch@available d-1-1.noarch@second

# case insensitive matching
nextjob
job noop selection /usr/lib64/* filelist,glob,nocase,flat
result jobs <inline>
#>job noop oneof a-1-1.noarch@available b-1-1.noarch@available c-1-1.noarch@available d-1-1.noarch@second

nextjob
job noop selection /opt/d/* filelist,glob,flat
result jobs <inline>
#>job noop pkg d-1-1.noarch@second [noautoset]

# nothing matches
nextjob
job noop selection /usr/nothing/* filelist,glob,flat
result jobs <inline>
