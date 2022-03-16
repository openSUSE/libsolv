# Do not block an update because of a choice rule
#
#Rule #3:
#    !B-1-1.noarch [4] (w1)
#    A-1-1.noarch [2]I (w2)
#    Anew-2-1.noarch [6]
#Rule #4:
#    !B-1-1.noarch [3]I (w1)
#    A-1-1.noarch [2]I (w2)
#    Anew-2-1.noarch [6]
#
# ==> No choice rule for Rule#4!
# ==> Choice Rule for #3:
#    !B-1-1.noarch [4] (w1)
#    A-1-1.noarch [2]I (w2)
#
repo system 0 testtags <inline>
#>=Pkg: A 1 1 noarch
#>=Prv: libA
#>=Pkg: B 1 1 noarch
#>=Req: libA
repo available 0 testtags <inline>
#>=Pkg: A 2 1 noarch
#>=Pkg: Anew 2 1 noarch
#>=Prv: libA
system i686 rpm system

job update all packages
result transaction,problems <inline>
#>install Anew-2-1.noarch@available
#>upgrade A-1-1.noarch@system A-2-1.noarch@available
