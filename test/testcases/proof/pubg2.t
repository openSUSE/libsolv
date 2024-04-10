# test proof generation. Testcase adapted from the pubgrub documentation.

repo system 0 testtags <inline>
repo available 0 testtags <inline>
#>=Pkg: foo 1.0.0 0 i586
#>=Req: a = 1.0.0-0
#>=Req: b = 1.0.0-0
#>=Pkg: foo 1.1.0 0 i586
#>=Req: x = 1.0.0-0
#>=Req: y = 1.0.0-0
#>=Pkg: a 1.0.0 0 i586
#>=Req: b = 2.0.0-0
#>=Pkg: b 1.0.0 0 i586
#>=Pkg: b 2.0.0 0 i586
#>=Pkg: x 1.0.0 0 i586
#>=Req: y = 2.0.0-0
#>=Pkg: y 1.0.0 0 i586
#>=Pkg: y 2.0.0 0 i586
system i586 * system
job install name foo
result proof
result proof <inline>
#>proof 1948b081   0 learnt 7682393f6d13c0826a53c962786e088f
#>proof 1948b081   0: --> -foo-1.1.0-0.i586@available
#>proof 1948b081   1 job 5f4dd053becd2f9d6a53275ad6f03cca
#>proof 1948b081   1:      foo-1.1.0-0.i586@available
#>proof 1948b081   1: -->  foo-1.0.0-0.i586@available
#>proof 1948b081   2 pkg 8aae478b8be3daff223abda98d0644c4
#>proof 1948b081   2:     -foo-1.0.0-0.i586@available
#>proof 1948b081   2: -->  a-1.0.0-0.i586@available
#>proof 1948b081   3 pkg b01b3b447f2952882d1d301085806304
#>proof 1948b081   3:     -foo-1.0.0-0.i586@available
#>proof 1948b081   3: -->  b-1.0.0-0.i586@available
#>proof 1948b081   4 pkg 533dc5990cff22ebef619b9fec526447
#>proof 1948b081   4:     -b-1.0.0-0.i586@available
#>proof 1948b081   4: --> -b-2.0.0-0.i586@available
#>proof 1948b081   5 pkg 247d79a95bc693a27e8c163273139fd7
#>proof 1948b081   5:      b-2.0.0-0.i586@available
#>proof 1948b081   5:     -a-1.0.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   0 premise
#>proof 7682393f6d13c0826a53c962786e088f   0: -->  foo-1.1.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   1 pkg 3bd4c5353322e3b7581968129e9d0de9
#>proof 7682393f6d13c0826a53c962786e088f   1:     -foo-1.1.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   1: -->  x-1.0.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   2 pkg 5f0d9bbea0a633c28592c38dd2b16363
#>proof 7682393f6d13c0826a53c962786e088f   2:     -foo-1.1.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   2: -->  y-1.0.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   3 pkg f4f7527cef6b2a9a003aced565971733
#>proof 7682393f6d13c0826a53c962786e088f   3:     -y-1.0.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   3: --> -y-2.0.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   4 pkg 835e2619b83f4ab1a69ff64d0b58cf1d
#>proof 7682393f6d13c0826a53c962786e088f   4:      y-2.0.0-0.i586@available
#>proof 7682393f6d13c0826a53c962786e088f   4:     -x-1.0.0-0.i586@available
