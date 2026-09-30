# the last line of this file intentionally has no trailing newline
# (it must not be ignored by testcase_read, see issue #490)
repo system 0 testtags <inline>
#>=Pkg: a 1 1 noarch
repo available 0 testtags <inline>
#>=Pkg: b 1 1 noarch
#>=Pkg: c 1 1 noarch
system x86_64 rpm system
result transaction,problems <inline>
#>install b-1-1.noarch@available
#>install c-1-1.noarch@available
job install name b
job install name c