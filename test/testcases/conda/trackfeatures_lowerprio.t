repo system 0 empty
repo conda-forge 1 testtags <inline>
#>=Pkg: python 3.14.7 - noarch
#>=Flv: h0_106_cp314
#>=Bvr: 106
#>=Req: python_abi <CONDA> 3.14.*\20*_cp314
#>=Pkg: python 3.14.7 - noarch
#>=Flv: h0_6_cp314t
#>=Bvr: 6
#>=Trf: py_freethreading
#>=Req: python_abi <CONDA> 3.14.*\20*_cp314t
#>=Pkg: python_abi 3.14 - noarch
#>=Flv: 8_cp314
#>=Bvr: 8
#>=Pkg: python_abi 3.14 - noarch
#>=Flv: 8_cp314t
#>=Bvr: 8
#>=Trf: freethreading
#>=Pkg: numpy 2.5.2 - noarch
#>=Flv: py314_ft_1
#>=Bvr: 1
#>=Tim: 1788129265
#>=Req: python
#>=Req: python_abi <CONDA> 3.14.*\20*_cp314t
#>=Pkg: numpy 2.5.2 - noarch
#>=Flv: py314_gil_1
#>=Bvr: 1
#>=Tim: 1788129265
#>=Req: python
#>=Req: python_abi <CONDA> 3.14.*\20*_cp314
repo main 0 testtags <inline>
#>=Pkg: python_abi 3.14 - noarch
#>=Flv: 0_cp314
#>=Bvr: 0
#>=Pkg: python_abi 3.14 - noarch
#>=Flv: 0_cp314t
#>=Bvr: 0
system unset conda system

job install name numpy
result transaction,problems <inline>
#>install numpy-2.5.2-py314_gil_1.noarch@conda-forge
#>install python-3.14.7-h0_106_cp314.noarch@conda-forge
#>install python_abi-3.14-8_cp314.noarch@conda-forge
