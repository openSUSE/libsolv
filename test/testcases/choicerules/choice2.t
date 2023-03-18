#
# Test that updating package B will update package A
# instead of pulling in new package C
#
#Rule #5:
#    !A-2-2.noarch [5] (w1)
#    B-2-1.noarch [6] (w2)
#    C-2-1.noarch [8]
#Rule #7:
#    !A-2-1.noarch [4] (w1)
#    B-2-1.noarch [6] (w2)
#    C-2-1.noarch [8]
#Rule #8:
#    !A-1-1.noarch [2]I (w1)
#    B-1-1.noarch [3]I (w2)
#    C-1-1.noarch [7]
#
# ==> Choice Rule for #8:
#    !A-1-1.noarch [2]I (w1)
#    B-1-1.noarch [3]I (w2)
#
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Req: P = 1
#>=Pkg: B 1 1 noarch
#>=Prv: P = 1
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Req: P = 2
#>=Pkg: A 2 2 noarch
#>=Req: P = 2
#>=Pkg: B 2 1 noarch
#>=Prv: P = 2
#>=Pkg: C 1 1 noarch
#>=Prv: P = 1
#>=Pkg: C 2 1 noarch
#>=Prv: P = 2
system i686 rpm system

job update name B
result transaction,problems <inline>
#>upgrade A-1-1.noarch@system A-2-2.noarch@available
#>upgrade B-1-1.noarch@system B-2-1.noarch@available
