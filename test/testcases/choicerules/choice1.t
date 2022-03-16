#
#Rule #2:
#    !A-2-1.noarch [3] (w1)
#    B-2-1.noarch [4] (w2)
#    C-2-1.noarch [5]
#
# ==> Choice Rule
#    !A-2-1.noarch [3] (w1)
#    B-2-1.noarch [4] (w2)
#
repo system 0 testtags <inline>
#>=Pkg: B 1 1 noarch
#>=Prv: P = 1
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Req: P = 2
#>=Pkg: B 2 1 noarch
#>=Prv: P = 2
#>=Pkg: C 2 1 noarch
#>=Prv: P = 2
system i686 rpm system

job install name A
result transaction,problems <inline>
result transaction,problems <inline>
#>install A-2-1.noarch@available
#>upgrade B-1-1.noarch@system B-2-1.noarch@available
