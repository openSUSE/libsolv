# This tests that A is updated instead of Anew being installed
#
#Rule #4:
#    !B-2-2.noarch [11] (w1)
#    A-2-2.noarch [9] (w2)
#    Anew-2-2.noarch [10]
#Rule #11:
#    !B-2-1.noarch [8] (w1)
#    A-2-1.noarch [6] (w2)
#    Anew-2-1.noarch [7]
#
#Choice Rule for #4:
#    !B-2-2.noarch [11] (w1)
#    A-2-2.noarch [9] (w2)
#Choice Rule for #11
#    !B-2-1.noarch [8] (w1)
#    A-2-1.noarch [6] (w2)
#
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libA = 1-1
repo available 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libA = 1-1
#>=Pkg: Anew 1 1 noarch
#>=Prv: libA = 1-1
#>=Pkg: B 1 1 noarch
#>=Req: libA = 1-1
#>=Pkg: A 2 1 noarch
#>=Prv: libA = 2-1
#>=Pkg: Anew 2 1 noarch
#>=Prv: libA = 2-1
#>=Pkg: B 2 1 noarch
#>=Req: libA = 2-1
#>=Pkg: A 2 2 noarch
#>=Prv: libA = 2-2
#>=Pkg: Anew 2 2 noarch
#>=Prv: libA = 2-2
#>=Pkg: B 2 2 noarch
#>=Req: libA = 2-2
#>=Pkg: C 2 1 noarch
#>=Req: B = 2
system i686 rpm system

job install name C
result transaction,problems <inline>
#>install B-2-2.noarch@available
#>install C-2-1.noarch@available
#>upgrade A-1-1.noarch@system A-2-2.noarch@available
