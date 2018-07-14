feature complex_deps
repo available 0 testtags <inline>
#>=Pkg: A1 1 1 x86_64
#>=Prv: A
#>=Pkg: A2 1 1 x86_64
#>=Prv: A
#>=Pkg: B1 1 1 x86_64
#>=Prv: B
#>=Pkg: B2 1 1 x86_64
#>=Prv: B
#>=Pkg: C1 1 1 x86_64
#>=Prv: C
#>=Pkg: C2 1 1 x86_64
#>=Prv: C
#>=Pkg: D1 1 1 x86_64
#>=Prv: D
#>=Pkg: D2 1 1 x86_64
#>=Prv: D
#>=Pkg: X 1 1 x86_64
#>=Req: (A & B) | (C & D)
#>=Pkg: Y 1 1 x86_64
#>=Con: (A & B) | (C & D)
job install name X
result rules <inline>
#>rule job 3986285ed3e7fa05cb3367ca1e7f0d3d  X-1-1.x86_64@available
#>rule pkg 500f33fd32c5be8fb62896b334f580e7  B1-1-1.x86_64@available
#>rule pkg 500f33fd32c5be8fb62896b334f580e7  B2-1-1.x86_64@available
#>rule pkg 500f33fd32c5be8fb62896b334f580e7  D1-1-1.x86_64@available
#>rule pkg 500f33fd32c5be8fb62896b334f580e7  D2-1-1.x86_64@available
#>rule pkg 500f33fd32c5be8fb62896b334f580e7 -X-1-1.x86_64@available
#>rule pkg 551b370e0a9430e9fcb2608b73b2d6f1  A1-1-1.x86_64@available
#>rule pkg 551b370e0a9430e9fcb2608b73b2d6f1  A2-1-1.x86_64@available
#>rule pkg 551b370e0a9430e9fcb2608b73b2d6f1  D1-1-1.x86_64@available
#>rule pkg 551b370e0a9430e9fcb2608b73b2d6f1  D2-1-1.x86_64@available
#>rule pkg 551b370e0a9430e9fcb2608b73b2d6f1 -X-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  B1-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  B2-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  C1-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a  C2-1-1.x86_64@available
#>rule pkg dee60af94c8ea49c9f99362dcb65806a -X-1-1.x86_64@available
#>rule pkg e524681ade2cf622db027a5d989023c4  A1-1-1.x86_64@available
#>rule pkg e524681ade2cf622db027a5d989023c4  A2-1-1.x86_64@available
#>rule pkg e524681ade2cf622db027a5d989023c4  C1-1-1.x86_64@available
#>rule pkg e524681ade2cf622db027a5d989023c4  C2-1-1.x86_64@available
#>rule pkg e524681ade2cf622db027a5d989023c4 -X-1-1.x86_64@available
nextjob
job install name Y
result rules <inline>
#>rule job 181d7955b2179c4ffdffedf0198f4807  Y-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -A1-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -B2-1-1.x86_64@available
#>rule pkg 05a449a52d4792a0b847f482f5d7b509 -Y-1-1.x86_64@available
#>rule pkg 57f43a628a3173b517f444966e291dcc -C2-1-1.x86_64@available
#>rule pkg 57f43a628a3173b517f444966e291dcc -D2-1-1.x86_64@available
#>rule pkg 57f43a628a3173b517f444966e291dcc -Y-1-1.x86_64@available
#>rule pkg 61248d82dae65a4a486fc7321a4e50ac -C2-1-1.x86_64@available
#>rule pkg 61248d82dae65a4a486fc7321a4e50ac -D1-1-1.x86_64@available
#>rule pkg 61248d82dae65a4a486fc7321a4e50ac -Y-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -A2-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -B2-1-1.x86_64@available
#>rule pkg 85eac3dcd30bb5f51ad50b6644455822 -Y-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -A1-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -B1-1-1.x86_64@available
#>rule pkg 8611a3d851adbd063a5d34a3ad4804e6 -Y-1-1.x86_64@available
#>rule pkg 919596a70a75b1cdd73c596397bf70e7 -C1-1-1.x86_64@available
#>rule pkg 919596a70a75b1cdd73c596397bf70e7 -D2-1-1.x86_64@available
#>rule pkg 919596a70a75b1cdd73c596397bf70e7 -Y-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -A2-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -B1-1-1.x86_64@available
#>rule pkg ba6ba0db8d7421f83f7df6cc4df6df7d -Y-1-1.x86_64@available
#>rule pkg da2fe506ceca44a0c6eafab14af37505 -C1-1-1.x86_64@available
#>rule pkg da2fe506ceca44a0c6eafab14af37505 -D1-1-1.x86_64@available
#>rule pkg da2fe506ceca44a0c6eafab14af37505 -Y-1-1.x86_64@available
