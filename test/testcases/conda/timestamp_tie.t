repo system 0 empty
repo available 0 testtags <inline>
#>=Pkg: dep 1 - noarch
#>=Flv: x
#>=Pkg: dep 1 - noarch
#>=Flv: y
#>=Pkg: foo 1 - noarch
#>=Flv: b_0
#>=Bvr: 0
#>=Tim: 1788129265
#>=Req: dep <CONDA> 1\20y
#>=Pkg: foo 1 - noarch
#>=Flv: a_0
#>=Bvr: 0
#>=Tim: 1788129265
#>=Req: dep <CONDA> 1\20x
#>=Pkg: bar 1 - noarch
#>=Flv: a_0
#>=Bvr: 0
#>=Tim: 1788129265
#>=Req: dep <CONDA> 1\20x
#>=Pkg: bar 1 - noarch
#>=Flv: b_0
#>=Bvr: 0
#>=Tim: 1788129265
#>=Req: dep <CONDA> 1\20y
system unset conda system

job install name foo
result transaction,problems <inline>
#>install dep-1-x.noarch@available
#>install foo-1-a_0.noarch@available

nextjob

job install name bar
result transaction,problems <inline>
#>install bar-1-a_0.noarch@available
#>install dep-1-x.noarch@available
