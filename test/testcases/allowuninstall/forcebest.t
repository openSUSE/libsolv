repo system 0 testtags <inline>
#>=Pkg: a 1 1 noarch
#>=Req: b = 1-1
#>=Pkg: b 1 1 noarch
repo available 0 testtags <inline>
#>=Pkg: a 2 1 noarch
#>=Req: b = 2-1
#>=Pkg: b 2 1 noarch

system x86_64 rpm system
disable pkg b-1-1.noarch@system
disable pkg b-2-1.noarch@available
job allowuninstall pkg a-1-1.noarch@system
job allowuninstall pkg b-1-1.noarch@system
job update name a [forcebest]
result transaction,problems <inline>
#>problem e6d3911d info nothing provides b = 2-1 needed by a-2-1.noarch
#>problem e6d3911d solution 0011b04f allow a-1-1.noarch@system
#>problem e6d3911d solution 44d189a0 erase a-1-1.noarch@system
