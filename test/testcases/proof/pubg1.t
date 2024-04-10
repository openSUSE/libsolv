# test proof generation. Testcase adapted from the pubgrub documentation.

repo system 0 testtags <inline>
repo available 0 testtags <inline>
#>=Pkg: menu 1.5.0 0 i586
#>=Req: dropdown >= 2.0.0
#>=Pkg: menu 1.4.0 0 i586
#>=Req: dropdown >= 2.0.0
#>=Pkg: menu 1.3.0 0 i586
#>=Req: dropdown >= 2.0.0
#>=Pkg: menu 1.2.0 0 i586
#>=Req: dropdown >= 2.0.0
#>=Pkg: menu 1.1.0 0 i586
#>=Req: dropdown >= 2.0.0
#>=Pkg: menu 1.0.0 0 i586
#>=Req: dropdown = 1.8.0-0
#>=Pkg: dropdown 2.3.0 0 i586
#>=Req: icons >= 2.0.0
#>=Pkg: dropdown 2.2.0 0 i586
#>=Req: icons >= 2.0.0
#>=Pkg: dropdown 2.1.0 0 i586
#>=Req: icons >= 2.0.0
#>=Pkg: dropdown 2.0.0 0 i586
#>=Req: icons >= 2.0.0
#>=Pkg: dropdown 1.8.0 0 i586
#>=Req: intl = 3.0.0-0
#>=Pkg: icons 2.0.0 0 i586
#>=Pkg: icons 1.0.0 0 i586
#>=Pkg: intl 5.0.0 0 i586
#>=Pkg: intl 4.0.0 0 i586
#>=Pkg: intl 3.0.0 0 i586
system i586 * system
job install name menu
job install name icons = 1.0.0-0
job install name intl = 5.0.0-0
result proof <inline>
#>proof 77cc0794   0 job a8f3723000d5bf17a40da5be2d55acb7
#>proof 77cc0794   0: -->  icons-1.0.0-0.i586@available
#>proof 77cc0794   1 job 849a236b2c0babcf452280f9329ffb60
#>proof 77cc0794   1: -->  intl-5.0.0-0.i586@available
#>proof 77cc0794   2 pkg 6c71dff8447cb37961072789ddd1fc28
#>proof 77cc0794   2:     -icons-1.0.0-0.i586@available
#>proof 77cc0794   2: --> -icons-2.0.0-0.i586@available
#>proof 77cc0794   3 pkg 75f23e7b139dc557bc73357bf8e40d1b
#>proof 77cc0794   3:     -intl-5.0.0-0.i586@available
#>proof 77cc0794   3: --> -intl-3.0.0-0.i586@available
#>proof 77cc0794   4 pkg 4c82e21bc96ae251873e1ba9feac331c
#>proof 77cc0794   4:      icons-2.0.0-0.i586@available
#>proof 77cc0794   4: --> -dropdown-2.0.0-0.i586@available
#>proof 77cc0794   5 pkg 96da3bd30552ae62ce3062277fe4930c
#>proof 77cc0794   5:      icons-2.0.0-0.i586@available
#>proof 77cc0794   5: --> -dropdown-2.1.0-0.i586@available
#>proof 77cc0794   6 pkg 470e5e3a3b2a41c4a0665bbd62efa27d
#>proof 77cc0794   6:      icons-2.0.0-0.i586@available
#>proof 77cc0794   6: --> -dropdown-2.2.0-0.i586@available
#>proof 77cc0794   7 pkg 55e8f978138411bc3de867ae0581e135
#>proof 77cc0794   7:      icons-2.0.0-0.i586@available
#>proof 77cc0794   7: --> -dropdown-2.3.0-0.i586@available
#>proof 77cc0794   8 pkg f0d9c6d1203c0f16e039a27badb251e7
#>proof 77cc0794   8:      intl-3.0.0-0.i586@available
#>proof 77cc0794   8: --> -dropdown-1.8.0-0.i586@available
#>proof 77cc0794   9 pkg 9791632a332a9e6f8346ee160d85c883
#>proof 77cc0794   9:      dropdown-2.0.0-0.i586@available
#>proof 77cc0794   9:      dropdown-2.1.0-0.i586@available
#>proof 77cc0794   9:      dropdown-2.2.0-0.i586@available
#>proof 77cc0794   9:      dropdown-2.3.0-0.i586@available
#>proof 77cc0794   9: --> -menu-1.1.0-0.i586@available
#>proof 77cc0794  10 pkg 6bfa015a79dbfd00986a5faddf302267
#>proof 77cc0794  10:      dropdown-2.0.0-0.i586@available
#>proof 77cc0794  10:      dropdown-2.1.0-0.i586@available
#>proof 77cc0794  10:      dropdown-2.2.0-0.i586@available
#>proof 77cc0794  10:      dropdown-2.3.0-0.i586@available
#>proof 77cc0794  10: --> -menu-1.2.0-0.i586@available
#>proof 77cc0794  11 pkg 26abf43463f15539b736f5ca8dbbba37
#>proof 77cc0794  11:      dropdown-2.0.0-0.i586@available
#>proof 77cc0794  11:      dropdown-2.1.0-0.i586@available
#>proof 77cc0794  11:      dropdown-2.2.0-0.i586@available
#>proof 77cc0794  11:      dropdown-2.3.0-0.i586@available
#>proof 77cc0794  11: --> -menu-1.3.0-0.i586@available
#>proof 77cc0794  12 pkg 5ecd7278981dd44581a4752a13729e01
#>proof 77cc0794  12:      dropdown-2.0.0-0.i586@available
#>proof 77cc0794  12:      dropdown-2.1.0-0.i586@available
#>proof 77cc0794  12:      dropdown-2.2.0-0.i586@available
#>proof 77cc0794  12:      dropdown-2.3.0-0.i586@available
#>proof 77cc0794  12: --> -menu-1.4.0-0.i586@available
#>proof 77cc0794  13 pkg c2215c6fc492d0a0c827256f885583b3
#>proof 77cc0794  13:      dropdown-2.0.0-0.i586@available
#>proof 77cc0794  13:      dropdown-2.1.0-0.i586@available
#>proof 77cc0794  13:      dropdown-2.2.0-0.i586@available
#>proof 77cc0794  13:      dropdown-2.3.0-0.i586@available
#>proof 77cc0794  13: --> -menu-1.5.0-0.i586@available
#>proof 77cc0794  14 pkg b06ef89ba49b91f25750b1f31b031487
#>proof 77cc0794  14:      dropdown-1.8.0-0.i586@available
#>proof 77cc0794  14: --> -menu-1.0.0-0.i586@available
#>proof 77cc0794  15 job 4412d6c4c17b045a23ce1666b49cf631
#>proof 77cc0794  15:      menu-1.0.0-0.i586@available
#>proof 77cc0794  15:      menu-1.1.0-0.i586@available
#>proof 77cc0794  15:      menu-1.2.0-0.i586@available
#>proof 77cc0794  15:      menu-1.3.0-0.i586@available
#>proof 77cc0794  15:      menu-1.4.0-0.i586@available
#>proof 77cc0794  15:      menu-1.5.0-0.i586@available
